"""Appearance-preserving simplification with a common screen-space error cut."""

from dataclasses import dataclass
from fnmatch import fnmatchcase
import pathlib
import sys

import numpy as np

from . import log
from .geometry import weld_keeping
from .import_settings import MESHLET_TRIANGLES, SettingsError
from .lit_mesh import POSITION_SCALE
from .meshopt import UNLIMITED_ERROR, PERMISSIVE, REGULARIZE, REGULARIZE_LIGHT, simplify_with_update
from .repair import repair
from .tessellate import split_marked_edges

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "device"))
import gfx_color  # noqa: E402  (path must be set up first)

SCALE_PERCENTILE = 90
BISECTION_STEPS = 64

# A step and a half of RGB565 is what keeps the meshlets sharing their
# vertices on the meshes measured: one step leaves the larger of them with
# more vertices than triangles.
SEAM_COLOUR_TOLERANCE = 1.5 * np.array(gfx_color.STEP)


def densify(p, tris, labels, max_edge, rounds=16):
    """Conforming splits until no edge is longer than max_edge; each piece
    keeps its triangle's label."""
    labels = np.asarray(labels)
    for _ in range(rounds):
        edges = np.sort(np.concatenate([tris[:, [0, 1]], tris[:, [1, 2]], tris[:, [2, 0]]]), axis=1)
        # Numeric keys preserve the lexicographic order of non-negative vertex pairs.
        keys = np.unique(edges[:, 0].astype(np.int64) * len(p) + edges[:, 1])
        edges = np.column_stack([keys // len(p), keys % len(p)])
        long = edges[np.linalg.norm(p[edges[:, 0]] - p[edges[:, 1]], axis=1) > max_edge]
        if len(long) == 0:
            break
        p, tris, _, parent = split_marked_edges(p, tris, set(map(tuple, long)))
        labels = labels[parent]
    return p, tris, labels


@dataclass
class Allocation:
    """Source ownership and caller-selected screen-error policy."""
    tri_o: object = None
    object_names: tuple = ()
    material_names: tuple = ()
    overrides: tuple = ()
    eyes: object = None
    focal_px: float = 1.0


def colour_weight(pos, deviation):
    """Weight per colour level in meshoptimizer's extent-normalized space."""
    extent = np.ptp(pos, axis=0).max()
    return deviation / (extent * max(gfx_color.STEP)) if extent > 0 else 0.0


def _label_after(tris_in, labels_in, kept, tris_out):
    """Each output triangle's label: the label most of its vertices carried
    in the input, read through the vertices the simplifier kept."""
    vertex_label = np.full(int(tris_in.max()) + 1, -1)
    for k in range(3):
        vertex_label[tris_in[:, k]] = labels_in
    corner = vertex_label[kept][tris_out]
    same01 = corner[:, 0] == corner[:, 1]
    return np.where(same01 | (corner[:, 0] == corner[:, 2]), corner[:, 0], corner[:, 1])


def merge_close_colours(q, rgb, tolerance):
    """rgb with each vertex that shares a position (a row of `q`) with an
    earlier vertex, in colour order, whose colour is within `tolerance` on
    every channel given that vertex's colour. What stays differs from every
    other colour at its position by more than the tolerance, so merging
    again changes nothing."""
    _, ids, count = np.unique(q, axis=0, return_inverse=True, return_counts=True)
    order = np.argsort(ids.reshape(-1), kind="stable")
    bounds = np.concatenate([[0], np.cumsum(count)])
    out = rgb.copy()
    for group in np.flatnonzero(count > 1):
        members = order[bounds[group]:bounds[group + 1]]
        kept = []
        for i in members[np.lexsort(rgb[members].T[::-1])]:
            near = next((c for c in kept if np.all(np.abs(rgb[i] - c) <= tolerance)), None)
            if near is None:
                kept.append(rgb[i])
            else:
                out[i] = near
    return out


def object_groups(pos, tris, tri_o):
    """Objects sharing a welded vertex form one pass, so no pass splits a shared edge."""
    from scipy.sparse import coo_matrix
    from scipy.sparse.csgraph import connected_components

    _, welded = weld_keeping(pos, tris)
    owners, inverse = np.unique(tri_o, return_inverse=True)
    corners = np.column_stack([welded.ravel(), np.repeat(inverse, 3)])
    corners = np.unique(corners, axis=0)
    shared = corners[1:, 0] == corners[:-1, 0]
    a, b = corners[:-1, 1][shared], corners[1:, 1][shared]
    graph = coo_matrix((np.ones(len(a)), (a, b)), shape=(len(owners), len(owners)))
    _, groups = connected_components(graph, directed=False)
    return groups[inverse]


def projected_scale(pos, eyes, focal_px):
    """Pixels per world unit at the nearest densified surface vertex."""
    if eyes is None or len(eyes) == 0:
        return 1.0
    from scipy.spatial import cKDTree

    distance = np.maximum(cKDTree(pos).query(eyes)[0], np.finfo(np.float32).eps)
    return float(np.percentile(focal_px / distance, SCALE_PERCENTILE))


def pass_overrides(objects, materials, overrides):
    """Apply each rule once per pass; fixed object selectors must cover it."""
    importance, fixed = 1.0, None
    for rule in overrides:
        names = materials if "material" in rule else objects
        pattern = rule.get("material", rule.get("object"))
        matches = [name for name in names if fnmatchcase(name, pattern)]
        if not matches:
            continue
        if "importance" in rule:
            importance *= rule["importance"]
        else:
            others = [name for name in objects if not fnmatchcase(name, pattern)]
            if "object" in rule and others:
                raise SettingsError(f"fixed override {pattern!r} also caps welded objects: {', '.join(others)}")
            fixed = (fixed or 0) + rule["triangles"]
    return importance, fixed


def simplify(pos, rgb, tris, labels, triangles, *, colour_deviation, seal_seams=False,
             position_scale=POSITION_SCALE, allocation=None):
    """Allocate a fixed triangle budget with one absolute screen-error cut.
    Funkhouser and Séquin trade per-object benefit against per-frame cost;
    here log-space bisection finds a shared cut for a variant's budget.
    Welded objects share a pass. Colour costs colour_deviation world units
    per panel step, independent of the triangle ceiling. Stop within one
    meshlet below budget; retained vertices carry the returned labels."""
    allocation = Allocation() if allocation is None else allocation
    tri_o, object_names = allocation.tri_o, allocation.object_names
    material_names, overrides = allocation.material_names, allocation.overrides
    eyes, focal_px = allocation.eyes, allocation.focal_px
    options = REGULARIZE | PERMISSIVE
    labels = np.asarray(labels)
    tri_o = np.zeros(len(tris), dtype=np.int64) if tri_o is None else np.asarray(tri_o)
    if seal_seams:
        pos, rgb, tris, ownership = repair(pos, rgb, tris, np.column_stack([labels, tri_o]), 1.0 / position_scale)
        labels, tri_o = ownership.T
        options = REGULARIZE_LIGHT | PERMISSIVE
    welded, ids = weld_keeping(pos, np.arange(len(pos)))
    pos = welded[ids]
    groups = object_groups(pos, tris, tri_o)
    parts = []
    for group in np.unique(groups):
        sel = groups == group
        used, local = np.unique(tris[sel], return_inverse=True)
        local = local.reshape(-1, 3)
        names = [object_names[i] for i in np.unique(tri_o[sel])] if object_names else []
        material_ids, material_counts = np.unique(labels[sel], return_counts=True)
        materials = ([material_names[i] for i in material_ids[np.argsort(-material_counts, kind="stable")]]
                     if material_names else [])
        for rule in overrides:
            if "material" not in rule or "triangles" not in rule:
                continue
            if not any(fnmatchcase(name, rule["material"]) for name in materials):
                continue
            others = []
            for obj in np.unique(tri_o[sel]):
                own_materials = np.unique(labels[sel & (tri_o == obj)])
                if not any(fnmatchcase(material_names[i], rule["material"]) for i in own_materials):
                    others.append(object_names[obj])
            if others:
                raise SettingsError(f"fixed material override {rule['material']!r} also caps welded objects: "
                                    + ", ".join(others))
        importance, fixed = pass_overrides(names, materials, overrides)
        scale = projected_scale(pos[used], eyes, focal_px) * importance
        parts.append(dict(pos=pos[used], rgb=rgb[used], tris=local, labels=labels[sel], fixed=fixed,
                          scale=scale, objects=names, materials=materials, cache={}))
    for rule in overrides:
        selector = "material" if "material" in rule else "object"
        key = "materials" if selector == "material" else "objects"
        matched = [part for part in parts if any(fnmatchcase(name, rule[selector]) for name in part[key])]
        if not matched:
            raise SettingsError(f"override {rule[selector]!r} matches no {selector}")
        if selector == "material" and "triangles" in rule and len(matched) != 1:
            raise SettingsError(f"fixed material override {rule[selector]!r} matches several passes: "
                                + "; ".join(", ".join(part["objects"]) for part in matched))
    for part in parts:
        if part["fixed"] is not None and part["fixed"] > len(part["tris"]):
            log(f"  fixed override {part['objects']}: clamped {part['fixed']} to {len(part['tris'])} triangles")
            part["fixed"] = len(part["tris"])
        part["weight"] = colour_weight(part["pos"], colour_deviation)
    reserved = sum(part["fixed"] or 0 for part in parts)
    if reserved > triangles:
        raise SettingsError(f"fixed object overrides ({reserved}) exceed triangle budget {triangles}")

    def evaluate(part, threshold):
        fixed = part["fixed"]
        error = (UNLIMITED_ERROR if fixed is not None
                 else float(np.float32(min(UNLIMITED_ERROR, threshold / part["scale"]))))
        if error not in part["cache"]:
            part["cache"][error] = simplify_with_update(
                part["pos"], part["rgb"], part["tris"], fixed if fixed is not None else 0,
                part["weight"], options, target_error=error, absolute_error=True)
        return part["cache"][error]

    def evaluate_cut(threshold):
        result = [evaluate(part, threshold) for part in parts]
        return sum(len(item[2]) for item in result), result

    low, high = 0.0, float("inf")
    count, result = evaluate_cut(low)
    threshold = low
    if count > triangles:
        count, result = evaluate_cut(high)
        high = max((item[4] * part["scale"] for part, item in zip(parts, result)
                    if part["fixed"] is None), default=0.0)
        threshold = high
        if count > triangles:
            raise ValueError(f"minimum simplification has {count} triangles, above budget {triangles}")
        for _ in range(BISECTION_STEPS):
            if triangles - count <= MESHLET_TRIANGLES:
                break
            middle = float(np.sqrt(max(low, np.finfo(np.float32).tiny) * high))
            trial_count, trial = evaluate_cut(middle)
            if trial_count > triangles:
                low = middle
            else:
                high = middle
                if trial_count >= count:
                    count, result, threshold = trial_count, trial, middle

    count = sum(len(item[2]) for item in result)
    log(f"  error cut {threshold:.6g}, {count}/{triangles} triangles, shortfall {triangles - count}")
    out_pos, out_rgb, out_tris, out_labels = [], [], [], []
    base = 0
    for index, (part, (p, c, t, kept, _)) in enumerate(zip(parts, result)):
        out_pos.append(p)
        out_rgb.append(c)
        out_tris.append(t + base)
        out_labels.append(_label_after(part["tris"], part["labels"], kept, t))
        base += len(p)
        log(f"  pass {index} objects={part['objects']} materials={part['materials'][:3]}: "
            f"{len(part['tris'])} -> {len(t)} triangles, scale {part['scale']:.6g}")
    out_pos, out_rgb = np.concatenate(out_pos), np.concatenate(out_rgb)
    if seal_seams:
        rounded = np.clip(np.rint(out_rgb), 0, 255)
        out_rgb = merge_close_colours(np.round(out_pos * position_scale).astype(np.int64), rounded,
                                      SEAM_COLOUR_TOLERANCE)
    return out_pos, out_rgb, np.concatenate(out_tris), np.concatenate(out_labels)

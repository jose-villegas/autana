"""Appearance-preserving simplification with projected-area prop budgets and one structure pass."""

from dataclasses import dataclass
from fnmatch import fnmatchcase
import pathlib
import sys

import numpy as np

from . import log
from .geometry import bounds_diagonal, triangle_areas, weld_keeping
from .import_settings import SettingsError, pass_overrides
from .lit_mesh import POSITION_SCALE
from .meshopt import PERMISSIVE, REGULARIZE, REGULARIZE_LIGHT, simplify_with_update
from .repair import repair
from .tessellate import split_marked_edges

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "device"))
import gfx_color  # noqa: E402  (path must be set up first)

SCALE_PERCENTILE = 90
# Bounds a free-standing single-object group relative to the whole source diagonal.
PROP_DIAGONAL_FRACTION = 0.05

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
    """Source ownership, reserved prop share and per-pass overrides."""
    tri_o: object = None
    object_names: tuple = ()
    material_names: tuple = ()
    overrides: tuple = ()
    eyes: object = None
    focal_px: float = 1.0
    props_share: float = 0.0
    source_diagonal: float | None = None


def colour_weight(pos, tris, budget, deviation):
    """Price one panel colour step against surface deviation, using the side of
    the surface area per budgeted triangle as meshoptimizer's attribute scale."""
    side = np.sqrt(triangle_areas(pos, tris).sum() / budget)
    return deviation / (side * max(gfx_color.STEP)) if side > 0 else 0.0


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


def prop_groups(pos, tris, tri_o, groups, source_diagonal=None):
    """Small single-object weld groups reserve detail independently of structure."""
    diagonal = bounds_diagonal(pos[np.unique(tris)]) if source_diagonal is None else source_diagonal
    limit = PROP_DIAGONAL_FRACTION * diagonal
    return {group for group in np.unique(groups)
            if len(np.unique(tri_o[groups == group])) == 1
            and bounds_diagonal(pos[np.unique(tris[groups == group])]) < limit}


def prop_budgets(weights, capacities, budget):
    """Capped proportional shares, with deterministic largest-remainder rounding."""
    weights, capacities = np.asarray(weights, dtype=float), np.asarray(capacities, dtype=int)
    shares = np.zeros(len(weights))
    remaining = min(budget, int(capacities.sum()))
    active = capacities > 0
    while np.any(active):
        w = weights[active]
        w = np.ones(len(w)) if not np.any(w) else w
        trial = remaining * w / w.sum()
        capped = trial >= capacities[active]
        ids = np.flatnonzero(active)
        if not np.any(capped):
            shares[ids] = trial
            break
        full = ids[capped]
        shares[full] = capacities[full]
        remaining -= int(capacities[full].sum())
        active[full] = False
    rounded = np.floor(shares).astype(int)
    extra = min(budget, int(capacities.sum())) - int(rounded.sum())
    order = np.argsort(-(shares - rounded), kind="stable")
    rounded[order[:extra]] += 1
    return rounded


def simplify_part(pos, rgb, tris, labels, tri_o, selected, object_names, material_names):
    """Compact a pass and retain its source names for overrides and allocation logs."""
    used, local = np.unique(tris[selected], return_inverse=True)
    material_ids, counts = np.unique(labels[selected], return_counts=True)
    return dict(pos=pos[used], rgb=rgb[used], tris=local.reshape(-1, 3), labels=labels[selected],
                objects=[object_names[i] for i in np.unique(tri_o[selected])] if object_names else [],
                materials=([material_names[i] for i in material_ids[np.argsort(-counts, kind="stable")]]
                           if material_names else []), selected=selected)


def simplify(pos, rgb, tris, labels, triangles, *, colour_deviation, seal_seams=False,
             position_scale=POSITION_SCALE, allocation=None):
    """Reserve a share for small free-standing props, split by projected area.
    Welded objects stay in one pass. Fixed overrides reserve their own ceilings;
    all remaining structure shares one global count-target simplification.
    Each pass prices colour using its own area and budget."""
    allocation = Allocation() if allocation is None else allocation
    tri_o, object_names = allocation.tri_o, allocation.object_names
    material_names, overrides = allocation.material_names, allocation.overrides
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
    props = prop_groups(pos, tris, tri_o, groups, allocation.source_diagonal)
    parts = []
    for group in np.unique(groups):
        sel = groups == group
        part = simplify_part(pos, rgb, tris, labels, tri_o, sel, object_names, material_names)
        names, materials = part["objects"], part["materials"]
        materials_by_object = {
            object_names[obj]: [material_names[i] for i in np.unique(labels[sel & (tri_o == obj)])]
            for obj in np.unique(tri_o[sel])
        } if object_names and material_names else {name: [] for name in names}
        importance, fixed = pass_overrides(names, materials_by_object, overrides)
        is_prop = group in props
        if not is_prop and any("importance" in rule and any(fnmatchcase(name, rule.get("object", rule.get("material")))
                               for name in (names if "object" in rule else materials)) for rule in overrides):
            raise SettingsError(f"importance override on structure {names}: it has nothing to split against")
        scale = projected_scale(part["pos"], allocation.eyes, allocation.focal_px) if is_prop else 1.0
        part.update(fixed=fixed, prop=is_prop, scale=scale,
                    weight=(scale * bounds_diagonal(part["pos"])) ** 2 * importance)
        parts.append(part)
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
    prop_share = int(triangles * allocation.props_share) if props else 0
    prop_allocated = 0
    for is_prop in (True, False):
        budget = prop_share if is_prop else triangles - prop_allocated
        pool = [part for part in parts if part["prop"] == is_prop]
        reserved = sum(part["fixed"] or 0 for part in pool)
        if reserved > budget:
            kind = "props" if is_prop else "structure"
            raise SettingsError(f"fixed overrides ({reserved}) exceed triangle budget {budget} in {kind} share")
        free = [part for part in pool if part["fixed"] is None]
        if is_prop:
            budgets = prop_budgets([part["weight"] for part in free], [len(part["tris"]) for part in free], budget - reserved)
            for part, count in zip(free, budgets):
                part["budget"] = int(count)
            prop_allocated = reserved + int(budgets.sum())
        elif free:
            selected = np.logical_or.reduce([part["selected"] for part in free])
            combined = simplify_part(pos, rgb, tris, labels, tri_o, selected, object_names, material_names)
            combined.update(fixed=None, prop=False, scale=1.0, budget=budget - reserved)
            parts = [part for part in parts if part["prop"] or part["fixed"] is not None] + [combined]
        for part in pool:
            if part["fixed"] is not None:
                part["budget"] = part["fixed"]
    log(f"  props share {allocation.props_share:g}, {len(props)} passes, budget {prop_share}/{triangles}")
    out_pos, out_rgb, out_tris, out_labels = [], [], [], []
    base = 0
    for index, part in enumerate(parts):
        budget = part["budget"]
        if budget == 0:
            continue
        weight = colour_weight(part["pos"], part["tris"], budget, colour_deviation)
        p, c, t, kept = simplify_with_update(part["pos"], part["rgb"], part["tris"], budget, weight, options)
        out_pos.append(p)
        out_rgb.append(c)
        out_tris.append(t + base)
        out_labels.append(_label_after(part["tris"], part["labels"], kept, t))
        base += len(p)
        kind = "prop" if part["prop"] else "structure"
        log(f"  pass {index} {kind} objects={part['objects']} materials={part['materials'][:3]}: "
            f"{len(part['tris'])} -> {len(t)} triangles (budget {budget}, scale {part['scale']:.6g})")
    if not out_pos:
        return pos[:0], rgb[:0], tris[:0], labels[:0]
    out_pos, out_rgb = np.concatenate(out_pos), np.concatenate(out_rgb)
    if seal_seams:
        rounded = np.clip(np.rint(out_rgb), 0, 255)
        out_rgb = merge_close_colours(np.round(out_pos * position_scale).astype(np.int64), rounded,
                                      SEAM_COLOUR_TOLERANCE)
    return out_pos, out_rgb, np.concatenate(out_tris), np.concatenate(out_labels)

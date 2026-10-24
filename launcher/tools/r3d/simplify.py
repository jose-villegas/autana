"""Appearance-preserving simplification of a lit mesh: split evenly, bake
colour per vertex (the caller does that), then one meshoptimizer pass per
part with colour as an attribute. Groups of materials can hold a reserved
share of the budget, so small detailed props are not starved by large
surfaces a global pass prefers to keep."""

from dataclasses import dataclass
from fnmatch import fnmatchcase
import pathlib
import sys

import numpy as np

from . import log
from .geometry import triangle_areas, weld_keeping
from .import_settings import SettingsError, pass_overrides
from .lit_mesh import POSITION_SCALE
from .meshopt import PERMISSIVE, REGULARIZE, REGULARIZE_LIGHT, simplify_with_update
from .repair import repair
from .tessellate import split_marked_edges

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "device"))
import gfx_color  # noqa: E402  (path must be set up first)

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


@dataclass
class Allocation:
    """Source ownership and fixed per-pass overrides."""
    tri_o: object = None
    object_names: tuple = ()
    material_names: tuple = ()
    overrides: tuple = ()


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


def simplify(pos, rgb, tris, labels, triangles, reserved=(), *, colour_deviation, seal_seams=False,
             position_scale=POSITION_SCALE, allocation=None):
    """Reserve material shares and fixed welded-object counts before the global pass.
    Colour is priced per pass; seam repair retains source ownership for overrides."""
    allocation = Allocation() if allocation is None else allocation
    tri_o = np.zeros(len(tris), dtype=np.int64) if allocation.tri_o is None else np.asarray(allocation.tri_o)
    options = REGULARIZE | PERMISSIVE
    if seal_seams:
        pos, rgb, tris, ownership = repair(pos, rgb, tris, np.column_stack([labels, tri_o]), 1.0 / position_scale)
        labels, tri_o = ownership.T
        options = REGULARIZE_LIGHT | PERMISSIVE
    labels = np.asarray(labels)
    parts, taken = [], np.zeros(len(tris), dtype=bool)
    if allocation.overrides:
        groups = object_groups(pos, tris, tri_o)
        matched = [0] * len(allocation.overrides)
        for group in np.unique(groups):
            sel = groups == group
            names = [allocation.object_names[i] for i in np.unique(tri_o[sel])]
            materials = {allocation.object_names[obj]:
                         [allocation.material_names[i] for i in np.unique(labels[sel & (tri_o == obj)])]
                         for obj in np.unique(tri_o[sel])}
            fixed = pass_overrides(names, materials, allocation.overrides)
            for index, rule in enumerate(allocation.overrides):
                candidates = names if "object" in rule else [name for values in materials.values() for name in values]
                matched[index] += any(fnmatchcase(name, rule.get("object", rule.get("material"))) for name in candidates)
            if fixed is not None:
                if fixed > triangles:
                    raise SettingsError(f"fixed overrides ({fixed}) exceed triangle budget {triangles}")
                if fixed > int(sel.sum()):
                    log(f"  fixed override {names}: clamped {fixed} to {int(sel.sum())} triangles")
                parts.append((sel, min(fixed, int(sel.sum()))))
                taken |= sel
        for rule, hits in zip(allocation.overrides, matched):
            pattern = rule.get("object", rule.get("material"))
            if not hits:
                raise SettingsError(f"override {pattern!r} matches no object or material")
            if "material" in rule and hits > 1:
                raise SettingsError(f"fixed material override {pattern!r} matches several passes")
    for group, share in reserved:
        sel = np.isin(labels, list(group)) & ~taken
        parts.append((sel, max(1, int(triangles * share))))
        taken |= sel
    if allocation.overrides and sum(n for _, n in parts) > triangles:
        raise SettingsError(f"fixed overrides and reserved shares exceed triangle budget {triangles}")
    minimum = 0 if allocation.overrides else 1
    parts.append((~taken, max(minimum, triangles - sum(n for _, n in parts))))

    out_pos, out_rgb, out_tris, out_labels = [], [], [], []
    base = 0
    for sel, budget in parts:
        if budget == 0 or not np.any(sel):
            continue
        sub = tris[sel]
        used, local = np.unique(sub, return_inverse=True)
        local = local.reshape(-1, 3)
        weight = colour_weight(pos[used], local, budget, colour_deviation)
        p, c, t, kept = simplify_with_update(pos[used], rgb[used], local, budget, weight, options)
        out_pos.append(p)
        out_rgb.append(c)
        out_tris.append(t + base)
        out_labels.append(_label_after(local, labels[sel], kept, t))
        base += len(p)
        log(f"  simplified {len(sub)} -> {len(t)} triangles (budget {budget}, colour weight {weight:.3g})")
    out_pos, out_rgb = np.concatenate(out_pos), np.concatenate(out_rgb)
    if seal_seams:
        rounded = np.clip(np.rint(out_rgb), 0, 255)
        out_rgb = merge_close_colours(np.round(out_pos * position_scale).astype(np.int64), rounded,
                                      SEAM_COLOUR_TOLERANCE)
    return out_pos, out_rgb, np.concatenate(out_tris), np.concatenate(out_labels)

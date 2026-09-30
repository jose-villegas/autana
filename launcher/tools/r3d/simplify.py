"""Appearance-preserving simplification of a lit mesh: split evenly, bake
colour per vertex (the caller does that), weld across materials, then one
meshoptimizer pass with colour as an attribute. Groups of materials can hold
a reserved share of the budget, so small detailed props are not starved by
large surfaces a global pass prefers to keep."""

import numpy as np

from . import log
from .lit_mesh import POSITION_SCALE
from .meshopt import PERMISSIVE, REGULARIZE, REGULARIZE_LIGHT, simplify_with_update
from .repair import repair
from .tessellate import split_marked_edges

# One RGB565 step on the sRGB channels is (8, 4, 8) levels. A step and a half
# is what keeps the meshlets sharing their vertices on the meshes measured: one
# step leaves the larger of them with more vertices than triangles.
SEAM_COLOUR_TOLERANCE = 1.5 * np.array([8, 4, 8])


def densify(p, tris, labels, max_edge, rounds=16):
    """Conforming splits until no edge is longer than max_edge; each piece
    keeps its triangle's label."""
    labels = np.asarray(labels)
    for _ in range(rounds):
        edges = np.unique(np.sort(np.concatenate([tris[:, [0, 1]], tris[:, [1, 2]], tris[:, [2, 0]]]), axis=1), axis=0)
        long = edges[np.linalg.norm(p[edges[:, 0]] - p[edges[:, 1]], axis=1) > max_edge]
        if len(long) == 0:
            break
        p, tris, _, parent = split_marked_edges(p, tris, set(map(tuple, long)))
        labels = labels[parent]
    return p, tris, labels


def weld_colours(pos, rgb, tris, labels, grid=1e-2):
    """One vertex per position, its colour the mean of the copies welded
    into it: the simplifier then sees one connected surface instead of seams
    at every crease and material edge."""
    key = np.round(pos / grid).astype(np.int64)
    _, first, inverse = np.unique(key, axis=0, return_index=True, return_inverse=True)
    inverse = inverse.reshape(-1)
    colour = np.zeros((len(first), 3))
    np.add.at(colour, inverse, rgb)
    colour /= np.bincount(inverse).astype(np.float64)[:, None]
    t = inverse[tris]
    keep = (t[:, 0] != t[:, 1]) & (t[:, 1] != t[:, 2]) & (t[:, 0] != t[:, 2])
    return pos[first], colour, t[keep], np.asarray(labels)[keep]


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


def simplify(pos, rgb, tris, labels, triangles, reserved=(), colour_weight=1.0, seal_seams=False,
             position_scale=POSITION_SCALE):
    """`reserved` is a list of (label set, share of `triangles`); what is
    left of the budget goes to every other label. `seal_seams` is the import
    option that joins pieces touching within one quantisation step
    (`1 / position_scale`, see repair.py) before the pass, simplifies with
    light regularizing, and afterwards gives vertices that end at one
    quantised position and differ by less than a panel colour step one
    colour. Returns pos, rgb (0..255 floats), tris and a label per
    triangle."""
    options = REGULARIZE | PERMISSIVE
    if seal_seams:
        pos, rgb, tris, labels = repair(pos, rgb, tris, labels, 1.0 / position_scale)
        options = REGULARIZE_LIGHT | PERMISSIVE
    labels = np.asarray(labels)
    parts, taken = [], np.zeros(len(tris), dtype=bool)
    for group, share in reserved:
        sel = np.isin(labels, list(group))
        parts.append((sel, max(1, int(triangles * share))))
        taken |= sel
    parts.append((~taken, max(1, triangles - sum(n for _, n in parts))))

    out_pos, out_rgb, out_tris, out_labels = [], [], [], []
    base = 0
    for sel, budget in parts:
        if not np.any(sel):
            continue
        sub = tris[sel]
        used, local = np.unique(sub, return_inverse=True)
        local = local.reshape(-1, 3)
        p, c, t, kept = simplify_with_update(pos[used], rgb[used], local, budget, colour_weight, options)
        out_pos.append(p)
        out_rgb.append(c)
        out_tris.append(t + base)
        out_labels.append(_label_after(local, labels[sel], kept, t))
        base += len(p)
        log(f"  simplified {len(sub)} -> {len(t)} triangles (budget {budget})")
    out_pos, out_rgb = np.concatenate(out_pos), np.concatenate(out_rgb)
    if seal_seams:
        rounded = np.clip(np.rint(out_rgb), 0, 255)
        out_rgb = merge_close_colours(np.round(out_pos * position_scale).astype(np.int64), rounded,
                                      SEAM_COLOUR_TOLERANCE)
    return out_pos, out_rgb, np.concatenate(out_tris), np.concatenate(out_labels)

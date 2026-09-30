"""Appearance-preserving simplification of a lit mesh: split evenly, bake
colour per vertex (the caller does that), weld across materials, then one
meshoptimizer pass with colour as an attribute. Groups of materials can hold
a reserved share of the budget, so small detailed props are not starved by
large surfaces a global pass prefers to keep."""

import numpy as np

from . import log
from .meshopt import PERMISSIVE, REGULARIZE, REGULARIZE_LIGHT, simplify_with_update
from .repair import repair
from .tessellate import split_marked_edges


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


def simplify(pos, rgb, tris, labels, triangles, reserved=(), colour_weight=1.0, watertight=False, position_scale=8):
    """`reserved` is a list of (label set, share of `triangles`); what is
    left of the budget goes to every other label. `watertight` is the import
    option that first joins pieces touching within one quantisation step
    (`1 / position_scale`, see repair.py) and simplifies with light
    regularizing. Returns pos, rgb (0..255 floats), tris and a label per
    triangle."""
    options = REGULARIZE | PERMISSIVE
    if watertight:
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
    return (np.concatenate(out_pos), np.concatenate(out_rgb), np.concatenate(out_tris),
            np.concatenate(out_labels))

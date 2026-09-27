"""Edge splitting that keeps a mesh conforming: each edge is split for every triangle sharing it or for none."""

import numpy as np

from .geometry import vertex_normals


def split_marked_edges(p, tris, marked):
    """Conforming split of every edge in `marked` (a set of sorted vertex
    pairs): decided per edge, so neighbours always agree. Returns the new
    positions, triangles and the midpoint index of each split edge."""
    p = list(map(tuple, p))
    midpoint = {}
    for a, b in marked:
        midpoint[(a, b)] = len(p)
        p.append(tuple((x + y) / 2 for x, y in zip(p[a], p[b])))

    def mid(a, b):
        return midpoint.get((a, b) if a < b else (b, a), -1)

    out, parent = [], []
    for index, (a, b, c) in enumerate(tris):
        ab, bc, ca = mid(a, b), mid(b, c), mid(c, a)
        splits = (ab >= 0) + (bc >= 0) + (ca >= 0)
        if splits == 0:
            pieces = [(a, b, c)]
        elif splits == 3:
            pieces = [(a, ab, ca), (ab, b, bc), (ca, bc, c), (ab, bc, ca)]
        else:
            while ab < 0:
                a, b, c = b, c, a
                ab, bc, ca = bc, ca, ab
            if splits == 1:
                pieces = [(a, ab, c), (ab, b, c)]
            elif bc >= 0:
                pieces = [(a, ab, c), (ab, b, bc), (ab, bc, c)]
            else:
                pieces = [(a, ab, ca), (ab, b, c), (ab, c, ca)]
        out += pieces
        parent += [index] * len(pieces)
    return np.array(p), np.array(out, dtype=np.int64), midpoint, np.array(parent, dtype=np.int64)


def adaptive_split(p, tris, attrs, brightness, min_edge, tolerance, rounds=12):
    """Splits an edge where the light changes along it - where its midpoint's
    brightness is off the mean of its ends by more than `tolerance` - or where
    it is longer than the smallest limit of the triangles sharing it. Uniformly
    lit areas keep their big triangles; shadow edges get the vertices they
    need. `attrs` holds a row per triangle, its length limit first; each
    piece of a split triangle inherits its row."""
    known = {}
    for _ in range(rounds):
        all_edges = np.sort(np.concatenate([tris[:, [0, 1]], tris[:, [1, 2]], tris[:, [2, 0]]]), axis=1)
        edges, owner = np.unique(all_edges, axis=0, return_inverse=True)
        edge_limit = np.full(len(edges), np.inf)
        np.minimum.at(edge_limit, owner.reshape(-1), np.tile(attrs[:, 0], 3))
        length = np.linalg.norm(p[edges[:, 0]] - p[edges[:, 1]], axis=1)
        normals = vertex_normals(p, tris)
        need = [v for v in np.unique(edges) if v not in known]
        if need:
            values = brightness(p[need], normals[need])
            known.update(zip(need, values))
        long_enough = length > min_edge
        cand = edges[long_enough]
        if len(cand) == 0:
            break
        mid_n = normals[cand[:, 0]] + normals[cand[:, 1]]
        mid_n /= np.maximum(np.linalg.norm(mid_n, axis=1, keepdims=True), 1e-12)
        mid_b = brightness((p[cand[:, 0]] + p[cand[:, 1]]) / 2, mid_n)
        ends = np.array([(known[a] + known[b]) / 2 for a, b in cand])
        marked = (np.abs(mid_b - ends) > tolerance) | (length[long_enough] > edge_limit[long_enough])
        if not np.any(marked):
            break
        chosen = cand[marked]
        p, tris, midpoint, parent = split_marked_edges(p, tris, set(map(tuple, chosen)))
        attrs = attrs[parent]
        for (a, b), value in zip(map(tuple, cand), mid_b):
            if (a, b) in midpoint:
                known[midpoint[(a, b)]] = value
    return p, tris, attrs

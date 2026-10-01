"""Edge splitting that keeps a mesh conforming: each edge is split for every triangle sharing it or for none."""

import numpy as np


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

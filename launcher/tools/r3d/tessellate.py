"""Edge splitting that keeps a mesh conforming: each edge is split for every triangle sharing it or for none."""

import numpy as np


def split_marked_edges(p, tris, marked):
    """Conforming split of every edge in `marked` (a set of sorted vertex
    pairs): decided per edge, so neighbours always agree. Returns the new
    positions, triangles and the midpoint index of each split edge."""
    p = np.asarray(p)
    if len(p) == 0:
        p = np.array([])
    midpoint = {}
    for a, b in marked:
        midpoint[(a, b)] = len(p) + len(midpoint)
    if midpoint:
        endpoints = np.array(list(midpoint), dtype=np.int64)
        positions = np.concatenate([p, (p[endpoints[:, 0]] + p[endpoints[:, 1]]) / 2])
    else:
        positions = p.copy()
    if len(tris) == 0:
        return positions, np.array([], dtype=np.int64), midpoint, np.array([], dtype=np.int64)

    edges = np.sort(tris[:, [[0, 1], [1, 2], [2, 0]]], axis=2)
    keys = edges[:, :, 0].astype(np.int64) * len(p) + edges[:, :, 1]
    mid = np.full((len(tris), 3), -1, dtype=np.int64)
    if midpoint:
        marked_keys = endpoints[:, 0] * len(p) + endpoints[:, 1]
        order = np.argsort(marked_keys)
        marked_keys = marked_keys[order]
        found = np.searchsorted(marked_keys, keys)
        bounded = np.minimum(found, len(marked_keys) - 1)
        matches = (found < len(marked_keys)) & (marked_keys[bounded] == keys)
        mid[matches] = len(p) + order[bounded[matches]]

    splits = (mid >= 0).sum(axis=1)
    rotation = np.argmax(mid >= 0, axis=1)
    corners = (np.arange(3)[None, :] + rotation[:, None]) % 3
    a, b, c = np.take_along_axis(tris, corners, axis=1).T
    ab, bc, ca = np.take_along_axis(mid, corners, axis=1).T
    pieces = np.empty((len(tris), 4, 3), dtype=np.int64)
    cases = (
        (splits == 0, ((a, b, c),)),
        (splits == 1, ((a, ab, c), (ab, b, c))),
        ((splits == 2) & (bc >= 0), ((a, ab, c), (ab, b, bc), (ab, bc, c))),
        ((splits == 2) & (bc < 0), ((a, ab, ca), (ab, b, c), (ab, c, ca))),
        (splits == 3, ((a, ab, ca), (ab, b, bc), (ca, bc, c), (ab, bc, ca))),
    )
    for mask, recipe in cases:
        pieces[mask, :len(recipe)] = np.stack([
            np.column_stack([corner[mask] for corner in triangle]) for triangle in recipe], axis=1)
    keep = np.arange(4)[None, :] <= splits[:, None]
    parent = np.repeat(np.arange(len(tris), dtype=np.int64), splits + 1)
    return positions, pieces[keep], midpoint, parent

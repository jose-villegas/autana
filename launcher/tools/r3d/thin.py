"""Thinning: keep a seeded share of a material's pieces and enlarge each kept piece so the kept ones cover what all
of them did (Cook et al. 2007)."""

import numpy as np

from .geometry import triangle_areas, weld_keeping

# Separate pieces of one cut-out material an object must hold to be an aggregate (foliage) thinned automatically.
AGGREGATE_PIECES_MIN = 32


def pieces(p, tri_v, tri_m):
    """One id per triangle: its connected piece, triangles of one material joined through shared welded corners."""
    from scipy.sparse import coo_matrix
    from scipy.sparse.csgraph import connected_components

    _, welded = weld_keeping(p, tri_v)
    corner = np.column_stack([welded.ravel(), np.repeat(tri_m, 3)])
    _, node = np.unique(corner, axis=0, return_inverse=True)
    count = len(tri_v)
    rows = np.repeat(np.arange(count), 3)
    graph = coo_matrix((np.ones(len(rows)), (rows, count + node.ravel())), shape=(count + node.max() + 1,) * 2)
    return connected_components(graph, directed=False)[1][:count]


def thin_keep(tri_m, tri_o, piece, rule, cut, ratio, names, skip=()):
    """Per triangle, the share of its pieces thinning keeps (1 where nothing is thinned): `rule`'s material at its
    keep, and `ratio` for each object holding at least AGGREGATE_PIECES_MIN pieces of one `cut` material."""
    keep = np.ones(len(tri_m))
    if ratio < 1:
        for material in np.flatnonzero(cut):
            for obj in np.unique(tri_o[tri_m == material]):
                own = (tri_m == material) & (tri_o == obj)
                if len(np.unique(piece[own])) >= AGGREGATE_PIECES_MIN:
                    keep[own] = ratio
    if rule is not None:
        keep[np.isin(tri_m, [i for i, name in enumerate(names) if name == rule.material])] = rule.keep
    keep[np.isin(tri_o, list(skip))] = 1
    return keep


def thin(p, tri_v, piece, keep, rng):
    """(kept per triangle, positions, triangles): each piece with keep below 1 survives with that chance, and a
    survivor is scaled by the square root of 1 / keep about its area centre so its area grows by 1 / keep."""
    kept = np.ones(len(tri_v), dtype=bool)
    thinned = keep < 1
    ids, first = np.unique(piece[thinned], return_index=True)
    share = keep[thinned][first]
    survives = rng.random(len(ids)) < share
    kept[thinned] = survives[np.searchsorted(ids, piece[thinned])]
    grown = thinned & kept
    if not np.any(grown):
        return kept, p, tri_v
    rows = np.flatnonzero(grown)
    corners = p[tri_v[rows]]
    area = triangle_areas(p, tri_v[rows])
    local = np.searchsorted(ids, piece[rows])
    weight = np.zeros(len(ids))
    np.add.at(weight, local, area)
    centre = np.zeros((len(ids), 3))
    np.add.at(centre, local, area[:, None] * corners.mean(axis=1))
    centre = centre / np.maximum(weight, 1e-30)[:, None]
    scale = 1 / np.sqrt(keep[rows])
    placed = centre[local][:, None] + (corners - centre[local][:, None]) * scale[:, None, None]
    out = tri_v.copy()
    out[rows] = len(p) + np.arange(3 * len(rows)).reshape(-1, 3)
    return kept, np.concatenate([p, placed.reshape(-1, 3)]), out

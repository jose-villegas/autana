"""Plain triangle-mesh arithmetic shared by the other modules."""

import math
from dataclasses import dataclass

import numpy as np
from scipy.spatial import cKDTree


def grouped_indices(keys):
    """Indices for each integer key, preserving input order within each group."""
    keys = np.asarray(keys)
    order = np.argsort(keys, kind="stable")
    found, starts = np.unique(keys[order], return_index=True)
    return dict(zip(found.tolist(), np.split(order, starts[1:])))


def bounds_diagonal(positions):
    """Length of the axis-aligned bounds diagonal, including degenerate objects."""
    return float(np.linalg.norm(np.ptp(positions, axis=0)))


def triangle_areas(p, tris):
    return 0.5 * np.linalg.norm(np.cross(p[tris[:, 1]] - p[tris[:, 0]], p[tris[:, 2]] - p[tris[:, 0]]), axis=1)


def face_normals(p, tris):
    """Each triangle's unit normal and its area."""
    normal = np.cross(p[tris[:, 1]] - p[tris[:, 0]], p[tris[:, 2]] - p[tris[:, 0]])
    length = np.linalg.norm(normal, axis=1)
    return normal / np.maximum(length, 1e-30)[:, None], 0.5 * length


def compact(p, tris):
    used, inverse = np.unique(tris, return_inverse=True)
    return p[used], inverse.reshape(-1, 3)


def distinct_corners(tris):
    """Which triangles retain three distinct vertex indices after welding."""
    return (tris[:, 0] != tris[:, 1]) & (tris[:, 1] != tris[:, 2]) & (tris[:, 0] != tris[:, 2])


def weld(p, tris, tolerance=1e-3):
    key = np.round(p / tolerance).astype(np.int64)
    _, first, inverse = np.unique(key, axis=0, return_index=True, return_inverse=True)
    tris = inverse.reshape(-1)[tris]
    good = distinct_corners(tris)
    return p[first], tris[good]


def weld_keeping(p, tris, tolerance=1e-3):
    """Merges coincident vertices across materials; every triangle stays."""
    key = np.round(p / tolerance).astype(np.int64)
    _, first, inverse = np.unique(key, axis=0, return_index=True, return_inverse=True)
    return p[first], inverse.reshape(-1)[tris]


def corner_normals(p, tris, crease_deg=40.0):
    """One normal per triangle corner, smoothed only across neighbours within
    the crease angle, so a column's edge stays sharp."""
    face_n = np.cross(p[tris[:, 1]] - p[tris[:, 0]], p[tris[:, 2]] - p[tris[:, 0]])
    face_area = np.linalg.norm(face_n, axis=1, keepdims=True)
    face_unit = face_n / np.maximum(face_area, 1e-12)
    cos_crease = math.cos(math.radians(crease_deg))
    corner_face = np.repeat(np.arange(len(tris)), 3)
    corner_vertex = tris.reshape(-1)
    order = np.argsort(corner_vertex, kind="stable")
    bounds = np.searchsorted(corner_vertex[order], np.arange(len(p) + 1))
    normals = np.zeros((len(corner_vertex), 3))
    for v in range(len(p)):
        corners = order[bounds[v] : bounds[v + 1]]
        faces = corner_face[corners]
        units = face_unit[faces]
        weighted = face_n[faces]
        similar = units @ units.T >= cos_crease
        n = similar.astype(np.float64) @ weighted
        normals[corners] = n / np.maximum(np.linalg.norm(n, axis=1, keepdims=True), 1e-12)
    return normals.reshape(-1, 3, 3)


def closest_point_on_triangles(q, a, b, c):
    """Ericson's closest-point test, vectorised; returns barycentrics."""
    ab, ac, ap = b - a, c - a, q - a
    d1, d2 = (ab * ap).sum(1), (ac * ap).sum(1)
    bp = q - b
    d3, d4 = (ab * bp).sum(1), (ac * bp).sum(1)
    cp = q - c
    d5, d6 = (ab * cp).sum(1), (ac * cp).sum(1)
    va = d3 * d6 - d5 * d4
    vb = d5 * d2 - d1 * d6
    vc = d1 * d4 - d3 * d2
    denom = np.where(np.abs(va + vb + vc) < 1e-30, 1e-30, va + vb + vc)
    v = vb / denom
    w = vc / denom
    bary = np.stack([1 - v - w, v, w], axis=1)

    def edge(t, i, j):
        t = np.clip(t, 0, 1)
        r = np.zeros((len(t), 3))
        r[:, i] = 1 - t
        r[:, j] = t
        return r

    with np.errstate(divide="ignore", invalid="ignore"):
        cases = [
            ((d1 <= 0) & (d2 <= 0), edge(np.zeros_like(d1), 0, 1)),
            ((d3 >= 0) & (d4 <= d3), edge(np.ones_like(d1), 0, 1)),
            ((d6 >= 0) & (d5 <= d6), edge(np.ones_like(d1), 0, 2)),
            ((vc <= 0) & (d1 >= 0) & (d3 <= 0), edge(d1 / (d1 - d3), 0, 1)),
            ((vb <= 0) & (d2 >= 0) & (d6 <= 0), edge(d2 / (d2 - d6), 0, 2)),
            ((va <= 0) & ((d4 - d3) >= 0) & ((d5 - d6) >= 0), edge((d4 - d3) / ((d4 - d3) + (d5 - d6)), 1, 2)),
        ]
    done = np.zeros(len(q), dtype=bool)
    for cond, value in cases:
        cond = cond & ~done
        bary[cond] = value[cond]
        done |= cond
    point = bary[:, 0:1] * a + bary[:, 1:2] * b + bary[:, 2:3] * c
    return bary, np.linalg.norm(point - q, axis=1)


# Centroids an upper bound is taken from; queries searched in one go, as every candidate pair is held at once.
SEEDS = 4
QUERY_CHUNK = 4096


@dataclass(frozen=True)
class TriangleSearch:
    """Triangles `corners` (n, 3, 3), their centroid tree, and groups of one size (so a huge triangle does not widen
    every ball) as (centroid tree, triangles, distance from each centroid to its farthest corner)."""
    corners: np.ndarray
    tree: cKDTree
    groups: tuple


def triangle_search(corners):
    """The nearest-triangle lookup for triangles `corners`."""
    corners = np.asarray(corners, dtype=np.float64)
    centre = corners.mean(axis=1)
    radius = np.linalg.norm(corners - centre[:, None], axis=2).max(axis=1)
    size = np.floor(np.log2(np.maximum(radius, 1e-30))).astype(int)
    groups = (np.flatnonzero(size == i) for i in np.unique(size))
    return TriangleSearch(corners, cKDTree(centre), tuple((cKDTree(centre[chosen]), chosen, radius[chosen])
                                                          for chosen in groups))


def nearest_triangles(points, search):
    """For each point, the nearest triangle of `search` (exact), the barycentrics of the nearest point on it and the
    distance to it. A triangle within d has its centroid within d plus its radius: all the candidates once any gives d."""
    points = np.asarray(points, dtype=np.float64).reshape(-1, 3)
    parts = [nearest_chunk(points[i:i + QUERY_CHUNK], search) for i in range(0, max(len(points), 1), QUERY_CHUNK)]
    return tuple(np.concatenate(part) for part in zip(*parts))


def nearest_chunk(points, search):
    corners = search.corners
    tri, bary, best = np.zeros(len(points), dtype=int), np.zeros((len(points), 3)), np.full(len(points), np.inf)

    def consider(query, found):
        """Keep each query's nearer of its best triangle so far and the triangles `found` for it."""
        here, distance = closest_point_on_triangles(points[query], *(corners[found, k] for k in range(3)))
        first = np.lexsort((distance, query))
        first = first[np.r_[True, query[first[1:]] != query[first[:-1]]][:len(first)]]
        update = first[distance[first] < best[query[first]]]
        tri[query[update]], bary[query[update]], best[query[update]] = found[update], here[update], distance[update]

    k = min(SEEDS, len(corners))
    seeds = search.tree.query(points, k=k)[1].reshape(len(points), k)
    for column in seeds.T:
        consider(np.arange(len(points)), column)
    for tree, owner, radius in search.groups:
        balls = tree.query_ball_point(points, best + radius.max(), return_sorted=False)
        query = np.repeat(np.arange(len(points)), [len(ball) for ball in balls])
        found = np.concatenate([[], *balls]).astype(int)
        keep = np.linalg.norm(points[query] - tree.data[found], axis=1) - radius[found] <= best[query]
        consider(query[keep], owner[found[keep]])
    return tri, bary, best


def majority(parts, values, count):
    """Per part 0..count-1, the value most of its entries hold (0 for a part with none)."""
    pairs, hits = np.unique(np.column_stack([parts, values]), axis=0, return_counts=True)
    best = np.zeros(count, dtype=np.int64)
    most = np.zeros(count, dtype=np.int64)
    for (part, value), number in zip(pairs, hits):
        if number > most[part]:
            best[part], most[part] = value, number
    return best

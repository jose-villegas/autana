"""Plain triangle-mesh arithmetic shared by the other modules."""

import math

import numpy as np


def triangle_areas(p, tris):
    return 0.5 * np.linalg.norm(np.cross(p[tris[:, 1]] - p[tris[:, 0]], p[tris[:, 2]] - p[tris[:, 0]]), axis=1)


def compact(p, tris):
    used, inverse = np.unique(tris, return_inverse=True)
    return p[used], inverse.reshape(-1, 3)


def weld(p, tris, tolerance=1e-3):
    key = np.round(p / tolerance).astype(np.int64)
    _, first, inverse = np.unique(key, axis=0, return_index=True, return_inverse=True)
    tris = inverse.reshape(-1)[tris]
    good = (tris[:, 0] != tris[:, 1]) & (tris[:, 1] != tris[:, 2]) & (tris[:, 0] != tris[:, 2])
    return p[first], tris[good]


def weld_keeping(p, tris, tolerance=1e-3):
    """Merges coincident vertices across materials; every triangle stays."""
    key = np.round(p / tolerance).astype(np.int64)
    _, first, inverse = np.unique(key, axis=0, return_index=True, return_inverse=True)
    return p[first], inverse.reshape(-1)[tris]


def snap_to_grid(p, tris, cell):
    key = np.floor(p / cell).astype(np.int64)
    _, inverse = np.unique(key, axis=0, return_inverse=True)
    inverse = inverse.reshape(-1)
    count = np.bincount(inverse)
    snapped = np.zeros((len(count), 3))
    np.add.at(snapped, inverse, p)
    snapped /= count[:, None]
    t = inverse[tris]
    t = t[(t[:, 0] != t[:, 1]) & (t[:, 1] != t[:, 2]) & (t[:, 0] != t[:, 2])]
    rolled = np.where((t[:, 1:2] < t[:, 0:1]) & (t[:, 1:2] < t[:, 2:3]), np.roll(t, -1, axis=1), t)
    rolled = np.where((rolled[:, 2:3] < rolled[:, 0:1]) & (rolled[:, 2:3] < rolled[:, 1:2]), np.roll(rolled, 1, axis=1), rolled)
    t = np.unique(rolled, axis=0)
    return compact(snapped, t)


def vertex_normals(p, tris):
    face = np.cross(p[tris[:, 1]] - p[tris[:, 0]], p[tris[:, 2]] - p[tris[:, 0]])
    n = np.zeros_like(p)
    for k in range(3):
        np.add.at(n, tris[:, k], face)
    return n / np.maximum(np.linalg.norm(n, axis=1, keepdims=True), 1e-12)


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

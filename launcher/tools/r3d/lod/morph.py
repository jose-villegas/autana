"""Where each vertex row of a cluster with a parent group lands on that
group's coarse surface: the geomorph target (Hoppe 1996, Strugar 2009) the
transform blends toward as the group's projected error nears the tolerance
(render/lod/r3d_lod.h).

Targets are measured from the mesh as written, its LMSH and LLOD, so every
mesh with levels gets them the same way. The simplifier keeps a subset of
the finer vertices, so each coarse vertex claims the fine vertex it was
kept from. A removed one goes to the nearest point on its parent group's
coarse triangles, preferring those facing its way. A border between groups
is locked, so its vertices are kept and land on themselves. A row also keeps
where it lands, a coarse triangle and its corner weights, so the draw blends
its colour toward the coarse colour there from whichever colours the mesh
draws with, a copy's colour set included.
"""

import numpy as np
from scipy.spatial import cKDTree

from r3d.geometry import closest_point_on_triangles
from r3d.mesh_asset import LOD_NONE

# Coarse triangles tried for each row, nearest middle first.
TARGET_CANDIDATES = 16
# Normals shorter than this are a degenerate triangle's, left at zero.
NORMAL_EPSILON = 1e-12
# Ticks of rounding allowed past the group's error when matching a kept vertex.
KEPT_SLACK_TICKS = 2.0
# Points tried for each coarse vertex when matching kept vertices.
KEPT_CANDIDATES = 8


def unit(vectors):
    return vectors / np.maximum(np.linalg.norm(vectors, axis=1, keepdims=True), NORMAL_EPSILON)


def area_normals(points, local):
    """Each of `points`' area-weighted normal over the triangles `local` (rows into points)."""
    corners = points[local].astype(np.float64)
    area = np.cross(corners[:, 1] - corners[:, 0], corners[:, 2] - corners[:, 0])
    around = np.zeros((len(points), 3))
    for k in range(3):
        np.add.at(around, local[:, k], area)
    return unit(around)


class Coarse:
    """One group's coarse triangles as drawn: corner positions and their
    triangle numbers in the mesh."""

    def __init__(self, corners, triangles):
        self.corners = corners.astype(np.float64)
        self.triangles = np.asarray(triangles, dtype=np.int64)
        a, b, c = self.corners[:, 0], self.corners[:, 1], self.corners[:, 2]
        self.normal = unit(np.cross(b - a, c - a))
        self.tree = cKDTree((a + b + c) / 3.0)

    def land(self, points, normals, reach):
        """(positions, triangle numbers, barycentrics) of the nearest point
        on a triangle facing each of `points`' `normals`, when one is within
        `reach` of the nearest point on any; else that nearest point."""
        k = min(TARGET_CANDIDATES, len(self.corners))
        _, candidates = self.tree.query(points, k=k)
        flat = candidates.reshape(len(points), k).reshape(-1)
        rows = np.repeat(points.astype(np.float64), k, axis=0)
        tri = self.corners[flat]
        bary, distance = closest_point_on_triangles(rows, tri[:, 0], tri[:, 1], tri[:, 2])
        distance = distance.reshape(len(points), k)
        facing = ((self.normal[flat] * np.repeat(normals, k, axis=0)).sum(axis=1) >= 0).reshape(len(points), k)
        near = distance.min(axis=1, keepdims=True)
        best = np.argmin(np.where(facing & (distance <= near + reach), distance, np.inf), axis=1)
        none = ~(facing & (distance <= near + reach)).any(axis=1)
        best[none] = np.argmin(distance[none], axis=1)
        picked = np.arange(len(points)) * k + best
        weights = bary[picked]
        out = np.rint((weights[:, :, None] * tri[picked]).sum(axis=1)).astype(np.int64)
        return out, self.triangles[flat[picked]], weights


def landing_weights(bary):
    """Two corner weights per row out of morph_asset.WEIGHT_WHOLE, the third the rest."""
    from r3d import morph_asset
    first = np.rint(bary[:, 0] * morph_asset.WEIGHT_WHOLE).astype(np.int64)
    second = np.minimum(np.rint(bary[:, 1] * morph_asset.WEIGHT_WHOLE).astype(np.int64),
                        morph_asset.WEIGHT_WHOLE - first)
    return first, second


def kept_corners(points, corners, reach):
    """Which of `corners` each of `points` is, or -1: the bake pushes a kept
    vertex outward by at most the group's error, so each coarse vertex is a
    point within `reach` of it, nearer pairs claiming first."""
    found = np.full(len(points), -1)
    taken = np.zeros(len(corners), dtype=bool)
    k = min(KEPT_CANDIDATES, len(points))
    distance, near = cKDTree(points).query(corners, k=k)
    distance, near = distance.reshape(len(corners), k), near.reshape(len(corners), k)
    for flat in np.argsort(distance, axis=None, kind="stable"):
        corner, rank = divmod(int(flat), k)
        point = near[corner, rank]
        if distance[corner, rank] > reach:
            break
        if not taken[corner] and found[point] < 0:
            found[point] = corner
            taken[corner] = True
    return found


def read_targets(mesh):
    """(positions, landing or None) per vertex row of a read mesh with levels
    (lit_mesh.read_lit_mesh): the target, and the coarse triangle and two
    corner weights where it lands, (rows, 3) ints; None for a face-coloured
    mesh, whose colours are per triangle. Rows of a cluster without a parent
    group target themselves, and rows at one position under one parent share
    a target."""
    pos = np.asarray(mesh.pos, dtype=np.int64)
    target = pos.copy()
    landing = None if mesh.face_colors is not None else np.zeros((len(pos), 3), dtype=np.int64)
    coarse, fine = {}, {}
    for (_, vc, tb, tc, *_), (own, parent) in zip(mesh.clusters, mesh.lod):
        if own != LOD_NONE:
            coarse.setdefault(own, []).append(np.arange(tb, tb + tc))
        if parent != LOD_NONE and vc:
            fine.setdefault(parent, []).append(mesh.tris[tb:tb + tc])
    for parent, parts in fine.items():
        tris = np.concatenate(parts)
        rows = np.unique(tris)
        points, at = np.unique(pos[rows], axis=0, return_inverse=True)
        at = at.reshape(-1)
        local = at[np.searchsorted(rows, tris)]
        normals = area_normals(points, local)
        numbers = np.concatenate(coarse[parent])
        corners = mesh.tris[numbers]
        surface = Coarse(pos[corners], numbers)
        unique = np.unique(pos[corners].reshape(-1, 3), axis=0)
        reach = float(mesh.groups[parent][4]) + KEPT_SLACK_TICKS
        kept = kept_corners(points, unique, reach)
        moved = surface.land(points, normals, reach)[0]
        moved[kept >= 0] = unique[kept[kept >= 0]]
        target[rows] = moved[at]
        if landing is not None:
            _, triangle, bary = surface.land(moved, normals, reach)
            first, second = landing_weights(bary)
            landing[rows] = np.c_[triangle, first, second][at]
    return target, landing
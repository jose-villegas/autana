"""Where each vertex row of a cluster with a parent group lands on that
group's coarse surface: the geomorph target (Hoppe 1996, Strugar 2009) the
transform blends toward as the group's projected error nears the tolerance
(render/lod/r3d_lod.h).

Targets are measured from the mesh as written, its LMSH and LLOD, so every
mesh with levels gets them the same way. The simplifier keeps a subset of
the finer vertices, so a kept vertex goes to its coarse vertex. A removed
one goes to the nearest point on its parent group's coarse triangles,
preferring those facing its way. A position shared with a cluster of
another parent, or of none, and the group's open border stay put, so
clusters blending by different amounts never crack. A row's colour target
is the coarse colour where it lands, by barycentrics, so the fully blended
fine cluster draws what the coarse one draws.
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
    """One group's coarse triangles as drawn: corner positions and colours (or None)."""

    def __init__(self, corners, colours):
        self.corners = corners.astype(np.float64)
        self.colours = None if colours is None else colours.astype(np.float64)
        a, b, c = self.corners[:, 0], self.corners[:, 1], self.corners[:, 2]
        self.normal = unit(np.cross(b - a, c - a))
        self.tree = cKDTree((a + b + c) / 3.0)

    def targets(self, points, normals):
        """(positions, colours or None) for rows at `points` with `normals`."""
        k = min(TARGET_CANDIDATES, len(self.corners))
        _, candidates = self.tree.query(points, k=k)
        flat = candidates.reshape(len(points), k).reshape(-1)
        rows = np.repeat(points.astype(np.float64), k, axis=0)
        tri = self.corners[flat]
        bary, distance = closest_point_on_triangles(rows, tri[:, 0], tri[:, 1], tri[:, 2])
        distance = distance.reshape(len(points), k)
        facing = ((self.normal[flat] * np.repeat(normals, k, axis=0)).sum(axis=1) >= 0).reshape(len(points), k)
        best = np.argmin(np.where(facing | ~facing.any(axis=1, keepdims=True), distance, np.inf), axis=1)
        picked = np.arange(len(points)) * k + best
        weights = bary[picked][:, :, None]
        out = np.rint((weights * tri[picked]).sum(axis=1)).astype(np.int64)
        if self.colours is None:
            return out, None
        colour = (weights * self.colours[flat[picked]]).sum(axis=1)
        return out, np.clip(np.rint(colour), 0, 255).astype(np.int64)


def open_border(tris, count):
    """Which of `count` points lie on an edge only one of `tris` uses."""
    pairs = np.sort(np.concatenate([tris[:, [0, 1]], tris[:, [1, 2]], tris[:, [2, 0]]]), axis=1)
    edges, uses = np.unique(pairs, axis=0, return_counts=True)
    border = np.zeros(count, dtype=bool)
    border[edges[uses == 1].reshape(-1)] = True
    return border


def kept_corners(points, corners, reach):
    """Which of `corners` each of `points` is, or -1: the bake pushes a kept
    vertex outward by at most the group's error, so a point and a coarse
    vertex within `reach` that are each other's nearest are one vertex."""
    found = np.full(len(points), -1)
    near_point = cKDTree(points).query(corners)[1]
    distance, near_corner = cKDTree(corners).query(points)
    mutual = (near_point[near_corner] == np.arange(len(points))) & (distance <= reach)
    found[mutual] = near_corner[mutual]
    return found


def shared_positions(mesh, pos):
    """The positions rows of clusters under more than one parent hold, no
    parent counting as one."""
    keys = np.unique(np.concatenate([np.c_[pos[vb:vb + vc], np.full(vc, parent)]
                                     for (vb, vc, *_), (_, parent) in zip(mesh.clusters, mesh.lod)]), axis=0)
    places, counts = np.unique(keys[:, :3], axis=0, return_counts=True)
    return places[counts > 1]


def read_targets(mesh):
    """(positions, colours or None) per vertex row of a read mesh with levels
    (lit_mesh.read_lit_mesh); rows of a cluster without a parent group target
    themselves, and rows at one position under one parent share a target."""
    pos = np.asarray(mesh.pos, dtype=np.int64)
    rgb = None if mesh.rgb is None else np.asarray(mesh.rgb, dtype=np.int64)
    target = pos.copy()
    colour = None if rgb is None else rgb.copy()
    coarse, fine = {}, {}
    for (_, vc, tb, tc, *_), (own, parent) in zip(mesh.clusters, mesh.lod):
        tris = mesh.tris[tb:tb + tc]
        if own != LOD_NONE:
            coarse.setdefault(own, []).append(tris)
        if parent != LOD_NONE and vc:
            fine.setdefault(parent, []).append(tris)
    shared = shared_positions(mesh, pos)
    for parent, parts in fine.items():
        tris = np.concatenate(parts)
        rows = np.unique(tris)
        points, at = np.unique(pos[rows], axis=0, return_inverse=True)
        at = at.reshape(-1)
        local = at[np.searchsorted(rows, tris)]
        normals = area_normals(points, local)
        corners = np.concatenate(coarse[parent])
        surface = Coarse(pos[corners], None if rgb is None else rgb[corners])
        unique = np.unique(pos[corners].reshape(-1, 3), axis=0)
        kept = kept_corners(points, unique, float(mesh.groups[parent][4]) + KEPT_SLACK_TICKS)
        moved = surface.targets(points, normals)[0]
        border = open_border(local, len(points))
        moved[border] = points[border]
        moved[kept >= 0] = unique[kept[kept >= 0]]
        if len(shared):
            still = cKDTree(shared).query(points)[0] == 0
            moved[still] = points[still]
        target[rows] = moved[at]
        if colour is not None:
            colour[rows] = surface.targets(moved, normals)[1][at]
    return target, colour

"""The fit's plane term: every start point is held to the source planes it starts on. A point inside a wall may
slide in it, a point on an edge along it, a corner not at all; none may leave its planes. The photometric loss
moves vertices to wherever colour fits, and in views that see a surface from afar sliding off its plane costs
nothing there; close up it shows as spikes and slivers."""
import numpy as np

from r3d.geometry import nearest_triangles, triangle_search, triangles_within

# A source face holds a start point when it is within the point's nearest face distance plus this many capped
# local start edge scales.
PLANE_REACH = 0.02
# Faces whose normals are this close (either side) are one plane.
PLANE_MERGE_DEG = 20.0
# Three independent planes fix a point; a fourth adds nothing.
MAX_PLANES = 3


def start_planes(points, scale, source):
    """(rows, normals): one row per start point and source plane it lies on, `scale` the points' local scales.
    A point's planes are its reachable faces' normals, largest face first, each merged into a kept one within
    PLANE_MERGE_DEG."""
    corners = np.asarray(source.p, np.float64)[np.asarray(source.tri_v)]
    points = np.asarray(points, np.float64)
    search = triangle_search(corners)
    best = nearest_triangles(points, search)[2]
    query, found = triangles_within(points, search, best + PLANE_REACH * np.asarray(scale, np.float64))
    cross = np.cross(corners[:, 1] - corners[:, 0], corners[:, 2] - corners[:, 0])
    area = np.linalg.norm(cross, axis=1)
    normal = cross / np.maximum(area, np.finfo(float).tiny)[:, None]
    keep = area[found] > 0
    query, found = query[keep], found[keep]
    order = np.lexsort((-area[found], query))
    query, found = query[order], found[order]
    merge = np.cos(np.radians(PLANE_MERGE_DEG))
    rows, normals = [], []
    bounds = np.r_[0, np.flatnonzero(np.diff(query)) + 1, len(query)]
    for first, last in zip(bounds[:-1], bounds[1:]):
        kept = []
        for face in found[first:last]:
            if all(abs(normal[face] @ plane) < merge for plane in kept):
                kept.append(normal[face])
                if len(kept) == MAX_PLANES:
                    break
        rows += [query[first]] * len(kept)
        normals += kept
    return np.asarray(rows, np.int64), np.asarray(normals, np.float64).reshape(-1, 3)


def plane_loss(points, start, rows, normals, scale):
    """Mean squared distance of each row's point from its start plane, in the point's local scales."""
    if not len(rows):
        return points.sum() * 0.0
    offset = ((points[rows] - start[rows]) * normals).sum(-1) / scale[rows]
    return offset.square().mean()

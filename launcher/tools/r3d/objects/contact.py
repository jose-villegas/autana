"""Deterministic vertex-to-surface contacts with border vertices on both objects."""

from dataclasses import dataclass
from functools import cache

import numpy as np
from scipy.spatial import cKDTree

from r3d.geometry import closest_point_on_triangles, grouped_indices
from r3d.lit_mesh import POSITION_SCALE
from r3d.repair import seal_tolerance


@dataclass(frozen=True)
class Contact:
    a: int
    b: int
    vertices_a: np.ndarray
    vertices_b: np.ndarray
    unshared: bool = False


# A vertex with more candidates than the batch limit runs alone.
PAIR_BATCH = 1 << 20
RADIUS_BINS_PER_OCTAVE = 4


def surface_border(pos, vertices, faces, tolerance):
    """(vertices within `tolerance` of a face, every corner of those faces, whether a touching vertex sits on no
    face corner); vertices are queried and tested PAIR_BATCH candidate pairs at a time."""
    vertices = np.asarray(vertices, dtype=int)
    if not len(vertices) or not len(faces):
        return set(), set(), False
    points, inverse = np.unique(pos[vertices], axis=0, return_inverse=True)
    return _surface_border(vertices, points, inverse, _face_query(pos, faces, tolerance), tolerance)


def _face_query(pos, faces, tolerance):
    corners = pos[faces]
    centres = corners.mean(axis=1)
    radii = np.linalg.norm(corners - centres[:, None], axis=2).max(axis=1)
    reach = radii.max() + tolerance
    # The ball uses double precision even when the radius screen below does not.
    # Leave room for rounding and squared-distance underflow in the radius screen.
    precision = np.finfo(centres.dtype)
    floor, slack = np.sqrt(precision.tiny), 1 + np.sqrt(precision.eps)
    groups = []
    thresholds = radii + tolerance
    band = np.floor(np.log2(np.maximum(thresholds, floor)) * RADIUS_BINS_PER_OCTAVE).astype(int)
    for rows in grouped_indices(band).values():
        radius = min(reach, max(floor, float(thresholds[rows].max()) * slack))
        tree = cKDTree(centres[rows])
        low = np.nextafter(tree.mins - radius, -np.inf)
        high = np.nextafter(tree.maxes + radius, np.inf)
        groups.append((rows, tree, radius, low, high))
    return faces, corners, centres, radii, groups


def _surface_border(vertices, points, inverse, query, tolerance):
    faces, corners, centres, radii, groups = query
    touched, shared = np.zeros(len(points), bool), np.zeros(len(points), bool)
    # All corners of a touched face must stay fixed: locking just the
    # incoming point would let the supporting surface move away from it.
    hit = np.zeros(len(faces), bool)
    for rows, tree, reach, low, high in groups:
        nearby = np.ones(len(points), dtype=bool)
        for axis, coords in enumerate(points.T):
            nearby &= (coords >= low[axis]) & (coords <= high[axis])
        owners = np.flatnonzero(nearby)
        if len(owners):
            _mark_border(points, owners, rows, tree, reach, corners, centres, radii, tolerance, touched, shared, hit)
    return set(vertices[touched[inverse]].tolist()), set(np.unique(faces[hit]).tolist()), bool(np.any(touched & ~shared))


def _mark_border(points, owners, rows, tree, reach, corners, centres, radii, tolerance, touched, shared, hit):
    query_points = points[owners]
    found = None
    if len(owners) * len(rows) <= PAIR_BATCH:
        found = tree.query_ball_point(query_points, reach, return_sorted=False)
        sizes = np.fromiter(map(len, found), dtype=int, count=len(owners))
    else:
        sizes = tree.query_ball_point(query_points, reach, return_length=True)
    ends = np.cumsum(sizes)
    begin = 0
    while begin < len(owners):
        before = ends[begin - 1] if begin else 0
        stop = max(begin + 1, int(np.searchsorted(ends, before + PAIR_BATCH, side='right')))
        if ends[stop - 1] > before:
            owner = np.repeat(owners[begin:stop], sizes[begin:stop])
            balls = found if found is not None else tree.query_ball_point(query_points[begin:stop], reach,
                                                                          return_sorted=False)
            candidate = rows[np.concatenate(balls).astype(int)]
            keep = np.linalg.norm(centres[candidate] - points[owner], axis=1) <= radii[candidate] + tolerance
            owner, candidate = owner[keep], candidate[keep]
            if len(owner):
                _, distance = closest_point_on_triangles(points[owner], *corners[candidate].transpose(1, 0, 2))
                near = distance <= tolerance
                owner, candidate = owner[near], candidate[near]
                touched[owner] = True
                on_corner = np.any(np.all(corners[candidate].astype(np.float32) ==
                                          points[owner, None].astype(np.float32), axis=2), axis=1)
                shared[owner[on_corner]] = True
                hit[candidate] = True
        begin = stop


def _overlapping_pairs(lows, highs, incident):
    order = np.argsort(lows[:, 0], kind="stable")
    ends = np.searchsorted(lows[order, 0], highs[order, 0], side="right")
    for start, (a, end) in enumerate(zip(order, ends), 1):
        others = order[start:end]
        others = others[(incident[a] | incident[others]) &
                        ~np.any(lows[a, 1:] > highs[others, 1:], axis=1) &
                        ~np.any(lows[others, 1:] > highs[a, 1:], axis=1)]
        yield from ((min(a, b), max(a, b)) for b in others)


def contact_pairs(pos, tris, tri_o, position_scale=POSITION_SCALE, among=None):
    """Surface contacts after a bounds sweep; `among` limits the incident objects."""
    groups = grouped_indices(tri_o)
    if not groups:
        return []
    tolerance = seal_tolerance(position_scale)
    owners = list(groups)
    objects = [tris[rows] for rows in groups.values()]
    vertices = [np.unique(faces).astype(int, copy=False) for faces in objects]
    points = [np.unique(pos[ids], axis=0, return_inverse=True) for ids in vertices]
    lows = np.array([p.min(axis=0) for p, _ in points]) - tolerance
    highs = np.array([p.max(axis=0) for p, _ in points]) + tolerance
    incident = np.array([among is None or obj in among for obj in owners])

    @cache
    def query(index):
        return _face_query(pos, objects[index], tolerance)

    edges = []
    for a, b in sorted(_overlapping_pairs(lows, highs, incident)):
        sa, sb, unshared_a = _surface_border(vertices[a], *points[a], query(b), tolerance)
        rb, ra, unshared_b = _surface_border(vertices[b], *points[b], query(a), tolerance)
        if sa or rb:
            edges.append(Contact(owners[a], owners[b], np.array(sorted(sa | ra), dtype=int),
                                 np.array(sorted(sb | rb), dtype=int), unshared_a or unshared_b))
    return edges


def contact_graph(pos, tris, tri_o, position_scale=POSITION_SCALE):
    return contact_pairs(pos, tris, tri_o, position_scale)

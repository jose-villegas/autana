"""Deterministic vertex-to-surface contacts with border vertices on both objects."""

from dataclasses import dataclass

import numpy as np
from scipy.spatial import cKDTree

from r3d.geometry import closest_point_on_triangles
from r3d.lit_mesh import POSITION_SCALE
from r3d.repair import seal_tolerance


@dataclass(frozen=True)
class Contact:
    a: int
    b: int
    vertices_a: np.ndarray
    vertices_b: np.ndarray
    unshared: bool = False


# Candidate (vertex, face) pairs tested in one batch: bounds the batch's memory, not its result.
PAIR_BATCH = 1 << 20


def _surface_border(pos, vertices, faces, tolerance):
    """(vertices within `tolerance` of a face, every corner of those faces, whether a touching vertex sits on no
    face corner); one batched closest-point test per PAIR_BATCH candidate pairs."""
    vertices = np.asarray(vertices, dtype=int)
    corners = pos[faces]
    centres = corners.mean(axis=1)
    radii = np.linalg.norm(corners - centres[:, None], axis=2).max(axis=1)
    tree = cKDTree(centres)
    points = pos[vertices]
    lists = tree.query_ball_point(points, radii.max() + tolerance)
    sizes = np.fromiter(map(len, lists), dtype=np.int64, count=len(lists))
    touched, shared, near_faces = np.zeros(len(vertices), bool), np.zeros(len(vertices), bool), []
    ends = np.cumsum(sizes)
    begin = 0
    while begin < len(vertices):
        before = ends[begin - 1] if begin else 0
        stop = max(begin + 1, int(np.searchsorted(ends, before + PAIR_BATCH, side='right')))
        owner = np.repeat(np.arange(begin, stop), sizes[begin:stop])
        candidate = np.fromiter((c for row in lists[begin:stop] for c in row), dtype=np.int64, count=len(owner))
        keep = np.linalg.norm(centres[candidate] - points[owner], axis=1) <= radii[candidate] + tolerance
        owner, candidate = owner[keep], candidate[keep]
        if len(owner):
            _, distance = closest_point_on_triangles(points[owner], *corners[candidate].transpose(1, 0, 2))
            near = distance <= tolerance
            owner, candidate = owner[near], candidate[near]
            touched[owner] = True
            on_corner = np.any(np.all(pos[faces[candidate]].astype(np.float32) ==
                                      points[owner, None].astype(np.float32), axis=2), axis=1)
            shared[owner[on_corner]] = True
            near_faces.append(faces[candidate].ravel())
        begin = stop
    # All corners of a touched face must stay fixed: locking just the
    # incoming point would let the supporting surface move away from it.
    target = set(np.concatenate(near_faces).tolist()) if near_faces else set()
    return set(vertices[touched].tolist()), target, bool(np.any(touched & ~shared))


def contact_pairs(pos, tris, tri_o, position_scale=POSITION_SCALE, among=None):
    """Surface contacts after a KD bounds broad phase; `among` limits the incident objects."""
    tolerance = seal_tolerance(position_scale)
    owners = np.unique(tri_o)
    objects = {int(obj): tris[tri_o == obj] for obj in owners}
    vertices = {obj: np.unique(faces) for obj, faces in objects.items()}
    if not len(owners):
        return []
    lows = np.array([pos[vertices[int(obj)]].min(axis=0) for obj in owners]) - tolerance
    highs = np.array([pos[vertices[int(obj)]].max(axis=0) for obj in owners]) + tolerance
    centres, reach = (lows + highs) / 2, np.linalg.norm(highs - lows, axis=1) / 2
    tree = cKDTree(centres)
    pairs = set()
    for at, obj in enumerate(owners):
        if among is not None and obj not in among:
            continue
        for other in tree.query_ball_point(centres[at], reach[at] + reach.max()):
            if at != other and not (np.any(lows[at] > highs[other]) or np.any(lows[other] > highs[at])):
                pairs.add(tuple(sorted((int(obj), int(owners[other])))))
    edges = []
    for a, b in sorted(pairs):
        sa, sb, unshared_a = _surface_border(pos, vertices[a], objects[b], tolerance)
        rb, ra, unshared_b = _surface_border(pos, vertices[b], objects[a], tolerance)
        if sa or rb:
            edges.append(Contact(a, b, np.array(sorted(sa | ra), dtype=int), np.array(sorted(sb | rb), dtype=int), unshared_a or unshared_b))
    return edges


def contact_graph(pos, tris, tri_o, position_scale=POSITION_SCALE):
    return contact_pairs(pos, tris, tri_o, position_scale)

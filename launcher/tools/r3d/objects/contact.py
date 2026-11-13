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


def _surface_border(pos, vertices, faces, tolerance):
    corners = pos[faces]
    centres = corners.mean(axis=1)
    radii = np.linalg.norm(corners - centres[:, None], axis=2).max(axis=1)
    tree = cKDTree(centres)
    source, target, unshared = set(), set(), False
    for vertex, candidates in zip(vertices, tree.query_ball_point(pos[vertices], radii.max() + tolerance)):
        candidates = np.asarray(candidates, dtype=int)
        candidates = candidates[np.linalg.norm(centres[candidates] - pos[vertex], axis=1) <= radii[candidates] + tolerance]
        if not len(candidates):
            continue
        tri = corners[candidates]
        _, distance = closest_point_on_triangles(np.broadcast_to(pos[vertex], (len(tri), 3)), *tri.transpose(1, 0, 2))
        near = candidates[distance <= tolerance]
        if len(near):
            source.add(int(vertex))
            shared = np.any(np.all(pos[faces[near]].astype(np.float32) == pos[vertex].astype(np.float32), axis=2))
            unshared |= not shared
            # All corners of a touched face must stay fixed: locking just the
            # incoming point would let the supporting surface move away from it.
            target.update(faces[near].ravel().tolist())
    return source, target, unshared


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

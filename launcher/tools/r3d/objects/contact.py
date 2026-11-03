"""Deterministic vertex-to-surface contacts with border vertices on both objects."""

from dataclasses import dataclass
from itertools import combinations

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


def contact_graph(pos, tris, tri_o, position_scale=POSITION_SCALE):
    tolerance = seal_tolerance(position_scale)
    objects = {int(obj): tris[tri_o == obj] for obj in np.unique(tri_o)}
    edges = []
    for a, b in combinations(objects, 2):
        ta, tb = objects[a], objects[b]
        va, vb = np.unique(ta), np.unique(tb)
        pa, pb = pos[va], pos[vb]
        if np.any(pa.min(axis=0) > pb.max(axis=0) + tolerance) or np.any(pb.min(axis=0) > pa.max(axis=0) + tolerance):
            continue
        sa, sb, unshared_a = _surface_border(pos, va, tb, tolerance)
        rb, ra, unshared_b = _surface_border(pos, vb, ta, tolerance)
        if sa or rb:
            edges.append(Contact(a, b, np.array(sorted(sa | ra), dtype=int), np.array(sorted(sb | rb), dtype=int), unshared_a or unshared_b))
    return edges

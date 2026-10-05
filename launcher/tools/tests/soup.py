"""Small triangle meshes for the r3d tests: just enough to build a floor and a wall and ask rays about them."""

import numpy as np

from r3d import ray_query
from r3d.ray_query import RayQuery

# Every ray query the tests make, in the bake too, traces one ray at a time: no LLVM needed.
ray_query.VARIANT = "scalar_rgb"


class Soup:
    def __init__(self, vertices, faces):
        self.vertices = np.asarray(vertices, dtype=np.float64)
        self.faces = np.asarray(faces, dtype=np.int64)

    def apply_translation(self, offset):
        self.vertices = self.vertices + np.asarray(offset, dtype=np.float64)


def box(extents):
    """An axis-aligned box centred on the origin with outward-facing triangles."""
    half = np.asarray(extents, dtype=np.float64) / 2.0
    corners = np.array([[x, y, z] for x in (-1, 1) for y in (-1, 1) for z in (-1, 1)], dtype=np.float64) * half
    quads = [(0, 1, 3, 2), (4, 6, 7, 5), (0, 4, 5, 1), (2, 3, 7, 6), (0, 2, 6, 4), (1, 5, 7, 3)]
    faces = [tri for a, b, c, d in quads for tri in ((a, b, c), (a, c, d))]
    soup = Soup(corners, faces)
    centre = corners.mean(axis=0)
    for index, (a, b, c) in enumerate(soup.faces):
        normal = np.cross(corners[b] - corners[a], corners[c] - corners[a])
        if np.dot(normal, corners[a] - centre) < 0:
            soup.faces[index] = (a, c, b)
    return soup


def concatenate(parts):
    offsets = np.cumsum([0] + [len(part.vertices) for part in parts[:-1]])
    return Soup(np.concatenate([part.vertices for part in parts]),
                np.concatenate([part.faces + offset for part, offset in zip(parts, offsets)]))


def rays(soup):
    """The ray queries of a mesh built here."""
    return RayQuery(soup.vertices, soup.faces)

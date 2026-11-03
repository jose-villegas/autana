"""Small triangle meshes for the r3d tests and the bake figures: just enough to build a floor and a wall and ask rays
about them."""

from types import SimpleNamespace

import numpy as np

from r3d import ray_query
from r3d.geometry import corner_normals
from r3d.light import LIGHT_GAMMA
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


# The bounce scene: a large floor facing +y at y = 0 and a large wall at x = 0 facing it, lit by a sun on the wall's
# side. Its bounced light is known in closed form at the wall's foot.
BIG = 400.0
SUN = {"type": "directional", "direction": [0.6, 1.0, 0.0], "color": [1.0, 1.0, 1.0], "intensity": 2.0}


def grey(albedo):
    """The Kd that decodes to `albedo`, as the export reads an untextured material."""
    return (albedo ** (1 / LIGHT_GAMMA),) * 3


def corridor(albedo=0.5, wall_faces_floor=True, wall_kd=None, wall_height=BIG):
    """The floor (material 0) and the wall (material 1) as a bake source; `wall_kd` colours the wall."""
    floor = [[0, 0, BIG], [BIG * 2, 0, BIG], [BIG * 2, 0, -BIG], [0, 0, -BIG]]
    wall = [[0, 0, -BIG], [0, 0, BIG], [0, wall_height, BIG], [0, wall_height, -BIG]]
    if wall_faces_floor:
        wall = wall[::-1]
    corners, tris, tri_m = [], [], []
    for index, quad in enumerate((floor, wall)):
        base = 4 * index
        corners += quad
        tris += [[base, base + 1, base + 2], [base, base + 2, base + 3]]
        tri_m += [index] * 2
    p, tri_v = np.array(corners, dtype=float), np.array(tris)
    source = SimpleNamespace(colors=None, p=p, uv=np.zeros((8, 2)), tri_v=tri_v, tri_t=tri_v, tri_m=np.array(tri_m),
                             names=["floor", "wall"],
                             materials={"floor": {"Kd": grey(albedo)}, "wall": {"Kd": wall_kd or grey(albedo)}},
                             textures=[None, None])
    source.corner_normals = corner_normals(p, tri_v)
    return source


def path_light(source=None, double=(), bounces=1, rays=256, boost=1.0, lights=(SUN,)):
    """The bake's bounced light over `source` (the corridor by default)."""
    from r3d.path_bake import PathLight

    return PathLight(source or corridor(), list(lights), set(double), SimpleNamespace(bounces=bounces, rays=rays), boost)


def floor_points(xs, height=0.0):
    """Points at x = `xs` and y = `height` (a number or one per point), facing +y as the corridor's floor does."""
    xs = np.asarray(xs, dtype=float)
    points = np.stack([xs, np.broadcast_to(height, xs.shape), np.zeros_like(xs)], axis=1)
    return points, np.tile([0.0, 1.0, 0.0], (len(xs), 1))


def sphere(seed):
    from scipy.spatial import ConvexHull

    points = np.random.default_rng(seed).normal(size=(120, 3))
    points /= np.linalg.norm(points, axis=1)[:, None]
    return points, ConvexHull(points).simplices


def edge_counts(faces):
    edges = np.sort(np.concatenate([faces[:, [0, 1]], faces[:, [1, 2]], faces[:, [2, 0]]]), axis=1)
    return np.unique(edges, axis=0, return_counts=True)[1]


def split_sphere(seed, budget, seam, report=None):
    from r3d.simplify import Allocation

    points, faces = sphere(seed)
    owners = (points[faces].mean(axis=1)[:, 0] > 0).astype(int)
    allocation = Allocation(tri_o=owners, object_names=('left', 'right'), material_names=('plain',), report=report,
                            meshes=({'match': {'object': 'left'}, 'triangles': budget, 'seam': seam},))
    return points, faces, owners, allocation

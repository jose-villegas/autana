"""Recover shared geometry under rigid placements, including reflections, never scale."""

import math
from collections import Counter
from dataclasses import dataclass, replace
from itertools import product

import numpy as np
from scipy.spatial import cKDTree

from r3d.geometry import bounds_diagonal
from r3d.lit_mesh import oriented

# A 381-object OBJ scene measured a matching plateau between 1e-3 and 1e-2 of the bounds diagonal.
MATCH_TOLERANCE = 1e-3


@dataclass(frozen=True)
class SourceObject:
    name: str
    positions: np.ndarray
    triangles: np.ndarray
    materials: np.ndarray


@dataclass(frozen=True)
class Instance:
    object_name: str
    linear: np.ndarray
    position: np.ndarray
    mirrored: bool
    residual: float
    triangle_order: tuple = ()


@dataclass(frozen=True)
class Shape:
    positions: np.ndarray
    triangles: np.ndarray
    materials: np.ndarray
    instances: tuple[Instance, ...]


def source_objects(positions, tri_v, tri_m, tri_o, object_names) -> list[SourceObject]:
    """Split triangles by source ownership, retaining vertices in first-use order."""
    positions, tri_v, tri_m, tri_o = map(np.asarray, (positions, tri_v, tri_m, tri_o))
    objects = []
    for index, name in enumerate(object_names):
        selected = tri_o == index
        triangles = tri_v[selected]
        if not len(triangles):
            continue
        vertices, first, inverse = np.unique(triangles, return_index=True, return_inverse=True)
        order = np.argsort(first)
        remap = np.argsort(order)
        objects.append(SourceObject(name, positions[vertices[order]], remap[inverse].reshape(-1, 3), tri_m[selected]))
    return objects


def place(points, linear, position):
    """points x linear^T + position, each coordinate one product per axis added in axis order: a matrix product's
    bits follow the host's BLAS kernels, and a copy placed by them would bake differently by host."""
    points = np.asarray(points, dtype=np.float64)
    linear = np.asarray(linear, dtype=np.float64)
    return points[:, :1] * linear[:, 0] + points[:, 1:2] * linear[:, 1] + points[:, 2:3] * linear[:, 2] + position


def placed_corners(shape, instance, matrix, position):
    """Eight local bounds corners under instance and renderer placement."""
    corners = np.array(list(product(*zip(shape.positions.min(axis=0), shape.positions.max(axis=0)))))
    return place(place(corners, instance.linear, instance.position), matrix, position)


def triangle_materials(triangles, materials):
    """Oriented triangle multiset, including materials and duplicate faces."""
    return Counter((oriented(face), int(material)) for face, material in zip(triangles, materials))


JACOBI_SWEEPS = 32
# A singular value below this share of the largest counts as zero: the object is flat (or a line) across it.
MIN_SINGULAR_RATIO = 1e-6


def symmetric_eigen(a):
    """Eigenvalues (descending) and unit eigenvectors (columns) of a symmetric 3x3, by cyclic Jacobi rotations
    (Golub and Van Loan 2013, 8.5): only products, sums and square roots, so every host gets the same bits."""
    a = [[float(a[i][j]) for j in range(3)] for i in range(3)]
    v = [[1.0 if i == j else 0.0 for j in range(3)] for i in range(3)]
    for _ in range(JACOBI_SWEEPS):
        if a[0][1] == 0.0 and a[0][2] == 0.0 and a[1][2] == 0.0:
            break
        for p, q in ((0, 1), (0, 2), (1, 2)):
            if a[p][q] == 0.0:
                continue
            theta = (a[q][q] - a[p][p]) / (2.0 * a[p][q])
            t = (1.0 if theta >= 0.0 else -1.0) / (abs(theta) + math.sqrt(theta * theta + 1.0))
            c = 1.0 / math.sqrt(t * t + 1.0)
            s = t * c
            _turn_columns(a, p, q, c, s)
            a[p], a[q] = [c * x - s * y for x, y in zip(a[p], a[q])], [s * x + c * y for x, y in zip(a[p], a[q])]
            _turn_columns(v, p, q, c, s)
    order = sorted(range(3), key=lambda i: -a[i][i])
    return [a[i][i] for i in order], np.array([[v[k][i] for i in order] for k in range(3)])


def _turn_columns(m, p, q, c, s):
    """Rotates columns p and q of every row of m by the angle with cosine c and sine s."""
    for row in m:
        row[p], row[q] = c * row[p] - s * row[q], s * row[p] + c * row[q]


def unit(vector):
    return vector / math.sqrt(math.fsum(vector * vector))


def cross(a, b):
    return np.array([a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]])


def rigid_fit(local, target, reflected=False):
    """Kabsch 1976 rotation fit, column-vector convention, computed so every host gets the same bits: exactly
    rounded sums, the singular vectors from symmetric_eigen() of H^T H, and the third left one as the cross product
    of the first two, which picks the same rotation as Kabsch's sign correction and holds for flat objects.

    Reflections are fitted only when a rotation fails residual or winding acceptance.
    """
    offset = target[0] - local[0]
    if not reflected and np.array_equal(local + offset, target):
        return np.eye(3), offset, 0.0
    mean_local = np.array([math.fsum(column) / len(local) for column in local.T])
    mean_target = np.array([math.fsum(column) / len(target) for column in target.T])
    a, b = local - mean_local, target - mean_target
    h = np.array([[math.fsum(a[:, i] * b[:, j]) for j in range(3)] for i in range(3)])
    hth = [[math.fsum(h[:, i] * h[:, j]) for j in range(3)] for i in range(3)]
    values, v = symmetric_eigen(hth)
    u1 = unit(place(v[:, :1].T, h, 0.0)[0]) if values[0] > 0.0 else np.array([1.0, 0.0, 0.0])
    u2 = place(v[:, 1:2].T, h, 0.0)[0] if values[1] > values[0] * MIN_SINGULAR_RATIO ** 2 else cross(
        u1, np.eye(3)[int(np.argmin(np.abs(u1)))])
    u2 = unit(u2 - math.fsum(u2 * u1) * u1)
    u = np.column_stack([u1, u2, cross(u1, u2)])
    det_v = math.fsum([v[0, 0] * (v[1, 1] * v[2, 2] - v[1, 2] * v[2, 1]),
                       -v[0, 1] * (v[1, 0] * v[2, 2] - v[1, 2] * v[2, 0]),
                       v[0, 2] * (v[1, 0] * v[2, 1] - v[1, 1] * v[2, 0])])
    v = v * [1.0, 1.0, (1.0 if det_v > 0.0 else -1.0) * (-1.0 if reflected else 1.0)]
    # Rounded to the float32 a copy is stored in, so a copy samples what its manifest holds.
    linear = np.array([[math.fsum(v[i] * u[j]) for j in range(3)] for i in range(3)]).astype(np.float32).astype(np.float64)
    position = (mean_target - place(mean_local[None], linear, 0.0)[0]).astype(np.float32).astype(np.float64)
    residual = float(np.sqrt(((place(local, linear, position) - target) ** 2).sum(axis=1)).max())
    return linear, position, residual


def match_object(shape, obj, tolerance):
    """Try corresponding topology first, then signed PCA frames and a bijective vertex map."""
    limit = tolerance * bounds_diagonal(obj.positions)
    target_faces = triangle_materials(obj.triangles, obj.materials)

    def fitted(mapping, reflected):
        linear, position, residual = rigid_fit(shape.positions, obj.positions[mapping], reflected)
        faces = mapping[shape.triangles]
        if reflected:
            faces = faces[:, ::-1]
        if residual <= limit and triangle_materials(faces, shape.materials) == target_faces:
            indices = {}
            for index in reversed(range(len(obj.triangles))):
                key = (oriented(obj.triangles[index]), int(obj.materials[index]))
                indices.setdefault(key, []).append(index)
            order = tuple(indices[(oriented(face), int(material))].pop()
                          for face, material in zip(faces, shape.materials))
            return Instance(obj.name, linear, position, reflected, residual, order)
        return None

    mappings = []
    if np.array_equal(shape.triangles, obj.triangles):
        mapping = np.arange(len(shape.positions))
        mappings.append(mapping)
        instance = fitted(mapping, False)
        if instance is not None:
            return instance
    local = shape.positions - shape.positions.mean(axis=0)
    target = obj.positions - obj.positions.mean(axis=0)
    _, axes_local = np.linalg.eigh(local.T @ local)
    _, axes_target = np.linalg.eigh(target.T @ target)
    tree = cKDTree(target)
    for signs in product((-1, 1), repeat=3):
        placed = local @ axes_local @ np.diag(signs) @ axes_target.T
        _, mapping = tree.query(placed)
        if len(np.unique(mapping)) == len(target):
            mappings.append(mapping)
            instance = fitted(mapping, False)
            if instance is not None:
                return instance
    for mapping in mappings:
        instance = fitted(mapping, True)
        if instance is not None:
            return instance
    return None


def match_instances(objects, tolerance=MATCH_TOLERANCE) -> list[Shape]:
    """Deterministic rigid matches in source order. Scaled copies remain separate shapes."""
    if not np.isfinite(tolerance) or tolerance < 0:
        raise ValueError('tolerance must be finite and nonnegative')
    shapes, buckets = [], {}
    for obj in objects:
        key = (len(obj.triangles), len(obj.positions), tuple(sorted(Counter(obj.materials).items())))
        for index in buckets.get(key, ()):
            shape = shapes[index]
            instance = match_object(shape, obj, tolerance)
            if instance is not None:
                shapes[index] = replace(shape, instances=(*shape.instances, instance))
                break
        else:
            centre = (obj.positions.min(axis=0) + obj.positions.max(axis=0)) / 2
            first = Instance(obj.name, np.eye(3), centre, False, 0.0, tuple(range(len(obj.triangles))))
            buckets.setdefault(key, []).append(len(shapes))
            shapes.append(Shape(obj.positions - centre, obj.triangles, obj.materials, (first,)))
    return shapes

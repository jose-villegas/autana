"""Recover shared geometry under rigid placements, including reflections, never scale."""

from collections import Counter
from dataclasses import dataclass, replace
from itertools import product

import numpy as np
from scipy.spatial import cKDTree

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


def bounds_diagonal(positions):
    """Length of the axis-aligned bounds diagonal, including degenerate objects."""
    return float(np.linalg.norm(np.ptp(positions, axis=0)))


def triangle_materials(triangles, materials):
    """Oriented triangle multiset, including materials and duplicate faces."""
    return Counter((oriented(face), int(material)) for face, material in zip(triangles, materials))


def rigid_fit(local, target, reflected=False):
    """Kabsch (docs/Citations.md#86), column-vector convention.

    Reflections are fitted only when a rotation fails residual or winding acceptance.
    """
    offset = target[0] - local[0]
    if not reflected and np.array_equal(local + offset, target):
        return np.eye(3), offset, 0.0
    mean_local, mean_target = local.mean(axis=0), target.mean(axis=0)
    u, _, vt = np.linalg.svd((local - mean_local).T @ (target - mean_target))
    correction = np.ones(3)
    correction[-1] = np.sign(np.linalg.det(vt.T @ u.T)) * (-1 if reflected else 1)
    linear = vt.T @ np.diag(correction) @ u.T
    position = mean_target - linear @ mean_local
    residual = float(np.linalg.norm(local @ linear.T + position - target, axis=1).max())
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
            return Instance(obj.name, linear, position, reflected, residual)
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
            first = Instance(obj.name, np.eye(3), centre, False, 0.0)
            buckets.setdefault(key, []).append(len(shapes))
            shapes.append(Shape(obj.positions - centre, obj.triangles, obj.materials, (first,)))
    return shapes

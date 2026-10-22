"""Recover shared geometry under rigid placements, including reflections, never scale."""

from collections import Counter
from dataclasses import dataclass, replace
from itertools import product

import numpy as np
from scipy.spatial import cKDTree

# Text sources round coordinates; matching plateaus between 1e-3 and 1e-2 of the bounds diagonal.
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


def triangle_materials(triangles, materials):
    """An unoriented triangle multiset, including materials and duplicate faces."""
    return Counter((tuple(sorted(map(int, face))), int(material)) for face, material in zip(triangles, materials))


def rigid_fit(local, target):
    """Orthogonal Procrustes (Kabsch, 1976), allowing determinant -1; rows store the linear map."""
    mean_local, mean_target = local.mean(axis=0), target.mean(axis=0)
    u, _, vt = np.linalg.svd((local - mean_local).T @ (target - mean_target))
    linear = (u @ vt).T
    position = mean_target - linear @ mean_local
    residual = float(np.linalg.norm(local @ linear.T + position - target, axis=1).max())
    return linear, position, residual


def match_object(shape, obj, tolerance):
    """Try corresponding topology first, then signed PCA frames and a bijective vertex map."""
    limit = tolerance * np.linalg.norm(np.ptp(obj.positions, axis=0))
    target_faces = triangle_materials(obj.triangles, obj.materials)

    def fitted(mapping):
        if triangle_materials(mapping[shape.triangles], shape.materials) != target_faces:
            return None
        linear, position, residual = rigid_fit(shape.positions, obj.positions[mapping])
        if residual <= limit:
            return Instance(obj.name, linear, position, bool(np.linalg.det(linear) < 0), residual)
        return None

    if np.array_equal(shape.triangles, obj.triangles):
        instance = fitted(np.arange(len(shape.positions)))
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
            instance = fitted(mapping)
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

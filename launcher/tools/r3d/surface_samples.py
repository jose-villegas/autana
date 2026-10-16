"""Random points on a source mesh's surface, outside the mesh bake's code."""

import numpy as np


def uniform_barycentric(rng, shape):
    """Barycentric weights (..., 3) of points uniform over a triangle's area."""
    s, t = np.sqrt(rng.random(shape)), rng.random(shape)
    return np.stack([1 - s, s * (1 - t), s * t], axis=-1)


def area_samples(source, triangles, count, rng):
    """`count` points spread by area over `triangles`, with their face normals."""
    a, b, c = (source.p[source.tri_v[triangles, k]] for k in range(3))
    cross = np.cross(b - a, c - a)
    area = np.linalg.norm(cross, axis=1)
    pick = rng.choice(len(triangles), count, p=area / area.sum())
    weights = uniform_barycentric(rng, count)
    points = weights[:, 0:1] * a[pick] + weights[:, 1:2] * b[pick] + weights[:, 2:3] * c[pick]
    return points, cross[pick] / area[pick, None]

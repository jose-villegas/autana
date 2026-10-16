"""Random points on a source mesh's surface, outside the mesh bake's code."""

import numpy as np


def area_samples(source, triangles, count, rng):
    """`count` points spread by area over `triangles`, with their face normals."""
    a, b, c = (source.p[source.tri_v[triangles, k]] for k in range(3))
    cross = np.cross(b - a, c - a)
    area = np.linalg.norm(cross, axis=1)
    pick = rng.choice(len(triangles), count, p=area / area.sum())
    s, t = np.sqrt(rng.random(count)), rng.random(count)
    points = (1 - s)[:, None] * a[pick] + (s * (1 - t))[:, None] * b[pick] + (s * t)[:, None] * c[pick]
    return points, cross[pick] / area[pick, None]

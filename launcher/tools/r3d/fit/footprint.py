"""The triangle a fitted renderer's budget affords, and the texture mip whose texel matches it.

Both are constant over the camera path: the source's area shared evenly by the fit's budget, and per textured
material the mip level whose texel spans that triangle's edge on the material's own surfaces.
"""

import numpy as np


def triangle_areas(p, tri_v):
    a, b, c = (p[tri_v[:, corner]] for corner in range(3))
    return 0.5 * np.linalg.norm(np.cross(b - a, c - a), axis=1)


def affordable_area(p, tri_v, budget):
    """World area of the triangle `budget` triangles afford over the source's surface."""
    return float(triangle_areas(np.asarray(p, dtype=np.float64), tri_v).sum()) / budget


def texel_edge(source, material):
    """Median world length of one texel's edge over `material`'s triangles, or None when it has no texture or
    no triangle with texture area."""
    texture = source.textures[material]
    if texture is None:
        return None
    chosen = np.flatnonzero(source.tri_m == material)
    world = triangle_areas(np.asarray(source.p, dtype=np.float64), source.tri_v[chosen])
    width, height = texture.size
    uv = np.asarray(source.uv, dtype=np.float64)[source.tri_t[chosen]] * (width, height)
    u, v = uv[:, 1] - uv[:, 0], uv[:, 2] - uv[:, 0]
    texels = 0.5 * np.abs(u[:, 0] * v[:, 1] - u[:, 1] * v[:, 0])
    textured = texels > 0
    if not textured.any():
        return None
    return float(np.median(np.sqrt(world[textured] / texels[textured])))


def mip_levels(source, area):
    """{material index: the mip level whose texel edge is nearest the affordable triangle's edge}, for each
    textured material; level 0 where a texel is already as large."""
    levels = {}
    for material, texture in enumerate(source.textures):
        edge = texel_edge(source, material)
        if edge is None:
            continue
        level = int(round(np.log2(np.sqrt(area) / edge)))
        levels[material] = int(np.clip(level, 0, len(texture.levels) - 1))
    return levels

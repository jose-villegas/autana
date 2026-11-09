"""Alpha-test source triangles into trimmed card masks and quadrant albedos."""

import numpy as np

from r3d.geometry import closest_point_on_triangles
from r3d.mask_asset import MASK_WIDTH as CARD_MASK_WIDTH
from r3d.ray_query import RayQuery

SUPERSAMPLES = 2
MIN_TEXELS = 2
KEEP_COVERAGE = 0.5
SLAB_MARGIN = 1.0
ALPHA_HIT_LIMIT = 16


def bake_pixels(card, query, positions, triangles, uv, tri_uv, texture, keep_alpha):
    size = card['hi'] - card['lo']
    texel_size = card.get("texel_size", size.max() / CARD_MASK_WIDTH)
    width, height = np.maximum(MIN_TEXELS, np.rint(size / texel_size).astype(int))
    gx, gy = np.meshgrid((np.arange(width * SUPERSAMPLES) + 0.5) / (width * SUPERSAMPLES),
                         (np.arange(height * SUPERSAMPLES) + 0.5) / (height * SUPERSAMPLES))
    xy = card['lo'] + np.column_stack([gx.ravel(), gy.ravel()]) * size
    origins = (card['c'] + xy[:, :1] * card['a'] + xy[:, 1:] * card['b'] +
               (card['depth'][1] + SLAB_MARGIN) * card['n'])
    directions = np.tile(-card['n'], (len(origins), 1))
    coverage = np.zeros(len(origins))
    alpha = np.zeros(len(origins))
    albedo = np.zeros((len(origins), 3))
    active = np.ones(len(origins), bool)
    for depth, (tri, ids, points) in enumerate(query.hit_rounds(origins, directions, active)):
        if depth >= ALPHA_HIT_LIMIT:
            break
        inside = ((points - card['c']) @ card['n']) >= card['depth'][0] - SLAB_MARGIN
        active[ids[~inside]] = False
        ids, tri, points = ids[inside], tri[inside], points[inside]
        if texture is None:
            rgba = np.ones((len(ids), 4))
        else:
            corners = positions[triangles[tri]]
            bary, _ = closest_point_on_triangles(points, corners[:, 0], corners[:, 1], corners[:, 2])
            tuv = (uv[tri_uv[tri]] * bary[..., None]).sum(axis=1)
            rgba = texture.sample(tuv, np.zeros(len(ids)))
        solid = rgba[:, 3] >= keep_alpha
        coverage[ids[solid]] = 1
        alpha[ids[solid]] = rgba[solid, 3]
        albedo[ids[solid]] = rgba[solid, :3] * rgba[solid, 3:4]
        active[ids[solid]] = False
    layout = (height, SUPERSAMPLES, width, SUPERSAMPLES)
    coverage = coverage.reshape(layout).mean(axis=(1, 3))
    alpha = alpha.reshape(layout).mean(axis=(1, 3))
    albedo = albedo.reshape(*layout, 3).mean(axis=(1, 3)) / np.maximum(alpha[..., None], np.finfo(float).eps)
    return coverage >= KEEP_COVERAGE, albedo, alpha


def bake_card(card, positions, triangles, uv, tri_uv, texture, keep_alpha, *, query=None):
    if not np.any(card['hi'] > card['lo']):
        return None  # no extent: nothing to cover, and no texel size to measure one
    query = RayQuery(positions, triangles) if query is None else query
    card = dict(card, texel_size=card.get('texel_size', max(card['hi'] - card['lo']) / CARD_MASK_WIDTH))
    mask, _, _ = bake_pixels(card, query, positions, triangles, uv, tri_uv, texture, keep_alpha)
    if not mask.any():
        return None
    y, x = np.nonzero(mask)
    height, width = mask.shape
    size = card['hi'] - card['lo']
    trimmed = dict(card, lo=card['lo'] + size * [x.min() / width, y.min() / height],
                   hi=card['lo'] + size * [(x.max() + 1) / width, (y.max() + 1) / height])
    mask, albedo, coverage = bake_pixels(trimmed, query, positions, triangles, uv, tri_uv, texture, keep_alpha)
    return (trimmed, mask, albedo, coverage) if mask.any() else None


def corner_colours(mask, albedo, coverage):
    height, width = mask.shape
    out = []
    fallback = np.average(albedo[mask], axis=0, weights=coverage[mask])
    for ys in (slice(0, (height + 1) // 2), slice(height // 2, height)):
        for xs in (slice(0, (width + 1) // 2), slice(width // 2, width)):
            member = mask[ys, xs]
            out.append(np.average(albedo[ys, xs][member], axis=0, weights=coverage[ys, xs][member])
                       if member.any() else fallback)
    return np.asarray(out)

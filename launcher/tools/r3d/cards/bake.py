"""Alpha-test source triangles into trimmed card masks and quadrant albedos."""

import numpy as np


SUPERSAMPLES = 2
MIN_TEXELS = 2
KEEP_COVERAGE = 0.5


def bake_pixels(card, query):
    size = card['hi'] - card['lo']
    texel_size = card["texel_size"]
    width, height = np.maximum(MIN_TEXELS, np.rint(size / texel_size).astype(int))
    gx, gy = np.meshgrid((np.arange(width * SUPERSAMPLES) + 0.5) / (width * SUPERSAMPLES),
                         (np.arange(height * SUPERSAMPLES) + 0.5) / (height * SUPERSAMPLES))
    xy = card['lo'] + np.column_stack([gx.ravel(), gy.ravel()]) * size
    origins = (card['c'] + xy[:, :1] * card['a'] + xy[:, 1:] * card['b'] +
               card['depth'][1] * card['n'])
    directions = np.tile(-card['n'], (len(origins), 1))
    coverage = np.zeros(len(origins))
    alpha = np.zeros(len(origins))
    albedo = np.zeros((len(origins), 3))
    active = np.ones(len(origins), bool)
    alpha_test = query.hit_filter
    for tri, ids, points in query.hit_rounds(origins, directions, active):
        inside = ((points - card['c']) @ card['n']) >= card['depth'][0]
        active[ids] = False
        ids, tri, points = ids[inside], tri[inside], points[inside]
        rgba = alpha_test.rgba(tri, points)
        coverage[ids] = 1
        alpha[ids] = rgba[:, 3]
        albedo[ids] = rgba[:, :3] * rgba[:, 3:4]
    layout = (height, SUPERSAMPLES, width, SUPERSAMPLES)
    coverage = coverage.reshape(layout).mean(axis=(1, 3))
    alpha = alpha.reshape(layout).mean(axis=(1, 3))
    albedo = albedo.reshape(*layout, 3).mean(axis=(1, 3)) / np.maximum(alpha[..., None], np.finfo(float).eps)
    return coverage >= KEEP_COVERAGE, albedo, alpha


def bake_card(card, query):
    if not np.any(card['hi'] > card['lo']):
        return None  # no extent: nothing to cover, and no texel size to measure one
    mask, _, _ = bake_pixels(card, query)
    if not mask.any():
        return None
    y, x = np.nonzero(mask)
    height, width = mask.shape
    size = card['hi'] - card['lo']
    trimmed = dict(card, lo=card['lo'] + size * [x.min() / width, y.min() / height],
                   hi=card['lo'] + size * [(x.max() + 1) / width, (y.max() + 1) / height])
    mask, albedo, coverage = bake_pixels(trimmed, query)
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

"""Select billboard planes with the greedy projected-area score of Décoret et al. 2003."""

import numpy as np
from scipy.cluster.vq import kmeans2
from scipy.spatial import cKDTree

from r3d.cards import (CARD_AREA_COST, CARD_EPSILON, CARD_FLOOR_EPS2, CARD_LIMIT,
                      CARD_RADIUS, CANDIDATE_MIN_SAMPLES, CANDIDATE_PATCHES,
                      SAMPLES_PER_TRIANGLE, W_PENALTY)
from r3d.cards import CARD_MASK_TEXELS, CARD_MAX_SIDE, CARD_MIP_LEVELS
from r3d.cards.pages import PAGE_SIDE
from r3d.views.probes import FACE_LENS
from r3d.poses import camera_basis
from r3d.light import AlphaTest
from r3d.surface_samples import uniform_barycentric

NORMAL_MIN = 1e-9
REFIT_ALIGNMENT = 0.8
REFIT_MIN_SAMPLES = 10
CANDIDATE_SEED_LIMIT = 1 << 30
VISIBILITY_MARGIN = 1.0
AXIS_ALIGNMENT_MIN = 0.1


def samples(sub, rng, keep_alpha):
    corners = sub.p[sub.tri_v]
    cross = np.cross(corners[:, 1] - corners[:, 0], corners[:, 2] - corners[:, 0])
    lengths = np.linalg.norm(cross, axis=1)
    keep = lengths > NORMAL_MIN
    corners, cross, lengths = corners[keep], cross[keep], lengths[keep]
    bary = uniform_barycentric(rng, (len(corners), SAMPLES_PER_TRIANGLE))
    points = (corners[:, None] * bary[..., None]).sum(axis=2).reshape(-1, 3)
    normals = np.repeat(cross / lengths[:, None], SAMPLES_PER_TRIANGLE, axis=0)
    area = np.repeat(lengths / (2 * SAMPLES_PER_TRIANGLE), SAMPLES_PER_TRIANGLE)
    hits = np.repeat(np.flatnonzero(keep), SAMPLES_PER_TRIANGLE)
    opaque = AlphaTest(sub, keep_alpha)(hits, points)
    points, normals, area = points[opaque], normals[opaque], area[opaque]
    return points, normals, area


def candidates(points, normals, rng):
    patches = min(CANDIDATE_PATCHES, len(points) // CANDIDATE_MIN_SAMPLES)
    if not patches:
        return []
    _, labels = kmeans2(points, patches, seed=int(rng.integers(CANDIDATE_SEED_LIMIT)), minit='++')
    out = []
    for patch in range(patches):
        member = labels == patch
        if member.sum() < CANDIDATE_MIN_SAMPLES:
            continue
        p = points[member]
        centre = p.mean(axis=0)
        _, _, axes = np.linalg.svd(p - centre, full_matrices=False)
        mean_normal = normals[member].mean(axis=0)
        for normal in (axes[-1], mean_normal, *np.eye(3)):
            length = np.linalg.norm(normal)
            if length > NORMAL_MIN:
                normal = normal / length
                out.extend([(centre, normal), (centre, -normal)])
    return out


def probe_weights(points, faces, query, face_lens):
    """Projected pixel area per sample over training cube faces, through the source's alpha-tested query."""
    weight = np.zeros(len(points))
    for pose in faces:
        eye = np.asarray(pose[:3])
        right, up, forward = camera_basis(pose[3:])
        v = points - eye
        z = v @ forward
        inside = (z > 0) & (np.abs(v @ right) < face_lens * z) & (np.abs(v @ up) < face_lens * z)
        ids = np.flatnonzero(inside)
        if not len(ids):
            continue
        distance = np.linalg.norm(v[ids], axis=1)
        hit, t, _ = query.first_hits(np.repeat(eye[None], len(ids), axis=0), v[ids] / distance[:, None])
        ids = ids[~hit | (t >= distance - VISIBILITY_MARGIN)]
        weight[ids] += 1 / z[ids] ** 2
    seen = weight > 0
    if seen.any():
        weight /= weight[seen].mean()
    return weight


def plane_axes(normal):
    a = np.cross(normal, [0.0, 1.0, 0.0])
    if np.linalg.norm(a) <= AXIS_ALIGNMENT_MIN:
        a = np.cross(normal, [1.0, 0.0, 0.0])
    a /= np.linalg.norm(a)
    return a, np.cross(normal, a)


def score_of(points, normals, area, centre, normal, ids, epsilon, area_cost):
    distance = (points[ids] - centre) @ normal
    projected = area[ids] * np.abs(normals[ids] @ normal)
    valid = np.abs(distance) < epsilon
    miss = (distance >= epsilon) & (distance < 2 * epsilon)
    if not valid.any():
        return -np.inf, ids[valid]
    a, b = plane_axes(normal)
    p = points[ids[valid]]
    card_area = np.ptp(p @ a) * np.ptp(p @ b)
    score = (projected[valid].sum() - W_PENALTY * projected[miss].sum() -
             area_cost * card_area - CARD_FLOOR_EPS2 * epsilon ** 2)
    return score, ids[valid]


def card_from(centre, normal, points, epsilon):
    projected = points - np.outer((points - centre) @ normal, normal)
    c = projected.mean(axis=0)
    _, _, axes = np.linalg.svd(projected - c, full_matrices=False)
    a = axes[0] - (axes[0] @ normal) * normal
    if np.linalg.norm(a) <= AXIS_ALIGNMENT_MIN:
        a = plane_axes(normal)[0]  # samples in a line along the normal: no spread to follow
    a /= np.linalg.norm(a)
    b = np.cross(normal, a)
    xy = np.column_stack([(projected - c) @ a, (projected - c) @ b])
    return dict(c=c, a=a, b=b, n=normal, lo=xy.min(axis=0), hi=xy.max(axis=0), depth=(-epsilon, epsilon))


def choose_cards(sampled, rng, *, views, query, face_lens=FACE_LENS, epsilon=CARD_EPSILON, area_cost=CARD_AREA_COST):
    points, normals, area = sampled
    if not len(points):
        return []
    weighted_area = area * probe_weights(points, views, query, face_lens)
    tree = cKDTree(points)
    base = candidates(points, normals, rng)
    near = {tuple(c): np.asarray(tree.query_ball_point(c, CARD_RADIUS), dtype=np.int64) for c, _ in base}
    covered = np.zeros(len(points), bool)
    chosen = []
    for _ in range(CARD_LIMIT):
        best, best_score, selected = None, 0.0, None
        for ci, (centre, normal) in enumerate(base):
            ids = near[tuple(centre)]
            ids = ids[~covered[ids]]
            if len(ids) < 3:
                continue
            score, valid = score_of(points, normals, weighted_area, centre, normal, ids, epsilon, area_cost)
            if score > best_score:
                best, best_score, selected = ci, score, valid
        if best is None:
            break
        c0, n0 = base.pop(best)
        p = points[selected]
        weight = area[selected] / area[selected].sum()
        centre = (p * weight[:, None]).sum(axis=0)
        _, _, axes = np.linalg.svd((p - centre) * np.sqrt(weight)[:, None], full_matrices=False)
        normal = axes[-1] if abs(axes[-1] @ n0) > REFIT_ALIGNMENT else n0
        normal = normal if normal @ n0 > 0 else -normal
        ids = np.asarray(tree.query_ball_point(centre, CARD_RADIUS), dtype=np.int64)
        ids = ids[~covered[ids]]
        valid = ids[np.abs((points[ids] - centre) @ normal) < epsilon]
        if len(valid) < REFIT_MIN_SAMPLES:
            centre, normal, valid = c0, n0, selected
        covered[valid] = True
        chosen.append(card_from(centre, normal, points[valid], epsilon))
    return chosen


def split_cards(cards, max_side=None):
    max_side = CARD_MAX_SIDE if max_side is None else max_side
    texel_size = (float(np.median([max(card["hi"] - card["lo"]) for card in cards])) / CARD_MASK_TEXELS
                  if cards else 0.0)
    if cards:
        max_side = min(max_side, (PAGE_SIDE - 2 * (1 << (CARD_MIP_LEVELS - 1))) * texel_size)
    pending = [dict(card, texel_size=texel_size) for card in cards]
    result = []
    while pending:
        card = pending.pop()
        size = card['hi'] - card['lo']
        axis = int(np.argmax(size))
        count = int(np.ceil(size[axis] / max_side))
        if count <= 1:
            result.append(card)
            continue
        edges = np.linspace(card['lo'][axis], card['hi'][axis], count + 1)
        for low, high in zip(edges[:-1], edges[1:]):
            lo, hi = card['lo'].copy(), card['hi'].copy()
            lo[axis], hi[axis] = low, high
            pending.append(dict(card, lo=lo, hi=hi))
    return result, len(result) - len(cards)

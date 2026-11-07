"""Select billboard planes with Décoret et al. 2003's greedy projected-area score."""

import numpy as np
from scipy.cluster.vq import kmeans2
from scipy.spatial import cKDTree

from r3d.cards import (CARD_AREA_COST, CARD_EPSILON, CARD_FLOOR_EPS2, CARD_LIMIT,
                      CARD_RADIUS, CANDIDATE_MIN_SAMPLES, CANDIDATE_PATCHES,
                      SAMPLES_PER_TRIANGLE, W_PENALTY)
from r3d.mask_asset import MASK_WIDTH
from r3d.poses import camera_basis, focal_px
from r3d.surface_samples import uniform_barycentric

NORMAL_MIN = 1e-9
REFIT_ALIGNMENT = 0.8
REFIT_MIN_SAMPLES = 10
CANDIDATE_SEED_LIMIT = 1 << 30
VISIBILITY_MARGIN = 1.0
AXIS_ALIGNMENT_MIN = 0.1


def samples(positions, triangles, rng, *, uv=None, tri_uv=None, texture=None, keep_alpha=None):
    corners = positions[triangles]
    cross = np.cross(corners[:, 1] - corners[:, 0], corners[:, 2] - corners[:, 0])
    lengths = np.linalg.norm(cross, axis=1)
    keep = lengths > NORMAL_MIN
    corners, cross, lengths = corners[keep], cross[keep], lengths[keep]
    bary = uniform_barycentric(rng, (len(corners), SAMPLES_PER_TRIANGLE))
    points = (corners[:, None] * bary[..., None]).sum(axis=2).reshape(-1, 3)
    normals = np.repeat(cross / lengths[:, None], SAMPLES_PER_TRIANGLE, axis=0)
    area = np.repeat(lengths / (2 * SAMPLES_PER_TRIANGLE), SAMPLES_PER_TRIANGLE)
    if texture is not None:
        sample_uv = (uv[tri_uv[keep]][:, None] * bary[..., None]).sum(axis=2).reshape(-1, 2)
        opaque = texture.sample(sample_uv, np.zeros(len(sample_uv)))[:, 3] >= keep_alpha
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


def path_weights(points, path, occluder):
    width, height, lens, near, poses = path
    weight = np.zeros(len(points))
    focal = focal_px(width, height, lens)
    horizontal, vertical = lens * np.array([width, height]) / min(width, height)
    for pose in poses:
        eye = pose[:3]
        right, up, forward = camera_basis(pose[3:])
        v = points - eye
        z = v @ forward
        inside = (z > near) & (np.abs(v @ right) < horizontal * z) & (np.abs(v @ up) < vertical * z)
        ids = np.flatnonzero(inside)
        if not len(ids):
            continue
        distance = np.linalg.norm(v[ids], axis=1)
        if occluder is not None:
            hit, t, _ = occluder.first_hits(np.repeat(eye[None], len(ids), axis=0), v[ids] / distance[:, None])
            ids = ids[~hit | (t > distance - VISIBILITY_MARGIN)]
        weight[ids] += (focal / z[ids]) ** 2
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
    a /= np.linalg.norm(a)
    b = np.cross(normal, a)
    xy = np.column_stack([(projected - c) @ a, (projected - c) @ b])
    return dict(c=c, a=a, b=b, n=normal, lo=xy.min(axis=0), hi=xy.max(axis=0), depth=(-epsilon, epsilon))


def choose_cards(positions, triangles, *, seed=0, epsilon=CARD_EPSILON, area_cost=CARD_AREA_COST,
                 path=None, occluder=None, uv=None, tri_uv=None, texture=None, keep_alpha=None):
    rng = np.random.default_rng(seed)
    points, normals, area = samples(np.asarray(positions), np.asarray(triangles), rng, uv=uv, tri_uv=tri_uv,
                                    texture=texture, keep_alpha=keep_alpha)
    if not len(points):
        return []
    weighted_area = area if path is None else area * path_weights(points, path, occluder)
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
    from r3d.cards import CARD_MAX_SIDE

    max_side = CARD_MAX_SIDE if max_side is None else max_side
    pending = [dict(card, texel_size=card.get("texel_size", max(card["hi"] - card["lo"]) / MASK_WIDTH))
               for card in cards]
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

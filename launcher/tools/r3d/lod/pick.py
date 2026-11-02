"""The level pick over placed meshes, and the one tolerance per frame that
meets a triangle budget.

A cluster is drawn at tolerance tau (pixels) when its own error, projected,
is at most tau and its group's is above it. One tau for every cluster of the
frame keeps the cut crack-free and is the cut that meets a triangle budget
at the least error (Wimmer and Schmalstieg 1998). An error projects at its
sphere's nearest point, as clusterlod's own rule does; with the eye inside
the sphere it projects at the near plane, so the finest level is drawn.

tau_from_histogram is the run-time way to find tau: each in-view cluster
adds its triangles at the bin where it starts drawing and takes them away
at the bin where its group takes over; the prefix sum is the triangles drawn
at every bin edge. tau_exact sorts every switch point instead, as the
reference the histogram is measured against.
"""

from typing import NamedTuple

import numpy as np

# Below half a pixel a coarser level is not worth its pop (Hoppe 1997).
TAU_FLOOR_PX = 0.5
# The histogram's bins, log-spaced from the floor to the coarsest tolerance it steers to.
TAU_CEILING_PX = 64.0
TAU_BINS = 32


class Placed(NamedTuple):
    """One mesh's levels under one transform: world = matrix @ local + position."""
    levels: object
    matrix: np.ndarray
    position: np.ndarray


class View(NamedTuple):
    """Every in-view cluster of one pose, over all placed meshes."""
    triangles: np.ndarray  # per cluster
    own_px: np.ndarray     # its own error, projected; 0 on the finest level
    group_px: np.ndarray   # its group's, projected; inf on a terminal group
    finest: np.ndarray     # whether it is a finest-level cluster


def frustum_planes(clip):
    """Left, right, bottom, top and near planes (a, b, c, d; inside is
    a x + b y + c z + d >= 0) of a world-to-clip matrix, rows normalised."""
    planes = np.array([clip[3] + clip[0], clip[3] - clip[0], clip[3] + clip[1], clip[3] - clip[1], clip[3]])
    return planes / np.linalg.norm(planes[:, :3], axis=1, keepdims=True)


def placed_spheres(spheres, placed):
    """Spheres (N x 4) of `placed`'s local mesh in world units; a radius,
    like an error, grows by the transform's largest scale."""
    scale = np.linalg.norm(placed.matrix, ord=2)
    return np.concatenate([spheres[:, :3] @ placed.matrix.T + placed.position, spheres[:, 3:] * scale], axis=1), scale


def projected_px(spheres, errors, eye, focal, near):
    """Errors (world units) at spheres seen from `eye`, in pixels."""
    distance = np.linalg.norm(spheres[:, :3] - eye, axis=1) - spheres[:, 3]
    with np.errstate(invalid="ignore"):
        return np.where(errors == 0, 0.0, errors * focal / np.maximum(distance, near))


def view(placements, eye, clip, focal, near):
    """The in-view clusters of every placed mesh at one pose."""
    planes = frustum_planes(clip)
    parts = []
    for placed in placements:
        levels = placed.levels
        clusters, scale = placed_spheres(levels.cluster_sphere, placed)
        shown = (clusters[:, :3] @ planes[:, :3].T + planes[:, 3] >= -clusters[:, 3:]).all(axis=1)
        groups, _ = placed_spheres(levels.group_sphere, placed)
        group_px = projected_px(groups, levels.group_error * scale, eye, focal, near)
        refined = levels.cluster_refined[shown]
        own = np.where(refined >= 0, group_px[np.maximum(refined, 0)], 0.0)
        parts.append(View(levels.cluster_triangles[shown], own, group_px[levels.cluster_group[shown]], refined < 0))
    return View(*(np.concatenate(column) for column in zip(*parts)))


def drawn(v, tau):
    """Whether each in-view cluster is drawn at `tau`; a terminal group's
    clusters stay drawn at any tau."""
    return (v.own_px <= tau) & ((tau < v.group_px) | np.isinf(v.group_px))


def triangles_at(v, tau):
    return int(v.triangles[drawn(v, tau)].sum())


def tau_exact(v, budget):
    """The least tau (at or above the floor) drawing at most `budget`
    triangles, or inf when even the coarsest levels draw more."""
    points = np.concatenate([v.own_px, v.group_px])
    steps = np.concatenate([v.triangles, -v.triangles])
    finite = np.isfinite(points)
    points, steps = points[finite], steps[finite]
    order = np.argsort(points, kind="stable")
    points, steps = points[order], np.cumsum(steps[order])
    last = np.r_[points[1:] != points[:-1], True]
    points, totals = points[last], steps[last]
    meets = np.flatnonzero(totals <= budget)
    if len(meets) == 0:
        return np.inf
    return max(float(points[meets[0]]), TAU_FLOOR_PX)


def bin_edges():
    return TAU_FLOOR_PX * (TAU_CEILING_PX / TAU_FLOOR_PX) ** (np.arange(TAU_BINS + 1) / TAU_BINS)


def tau_from_histogram(v, budget):
    """The least bin edge drawing at most `budget` triangles; the ceiling
    when none does (the budget's safety stop)."""
    edges = bin_edges()
    totals = np.zeros(TAU_BINS + 2, dtype=np.int64)
    # A cluster draws from the first edge at or above its own error until the first edge at or above its group's.
    np.add.at(totals, np.searchsorted(edges, v.own_px, side="left"), v.triangles)
    np.add.at(totals, np.searchsorted(edges, v.group_px, side="left"), -v.triangles)
    totals = np.cumsum(totals)[:TAU_BINS + 1]
    meets = np.flatnonzero(totals <= budget)
    return float(edges[meets[0]] if len(meets) else edges[-1])

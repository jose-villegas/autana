"""The level pick over placed meshes, and the one tolerance per frame that
meets a triangle budget.

A cluster is drawn at tolerance tau (pixels) when its own error, projected,
is at most tau and its group's is above it. One tau for every cluster of the
frame keeps the cut crack-free and is the cut that meets a triangle budget
at the least error: Wimmer and Schmalstieg 1998 derive it per object, here
it is over clusters. Refining by projected error is Hoppe 1997's. An error
projects at its sphere's nearest point, as clusterlod's own rule does; with
the eye inside the sphere it projects at the near plane, which keeps that
group fine at any tolerance short of a near-plane error. Culling is by
cluster box, as the device's cluster walk culls.

tau_from_histogram is the run-time way to find tau: each in-view cluster
adds its triangles at the bin where it starts drawing and takes them away
at the bin where its group takes over; the prefix sum is the triangles drawn
at every bin edge. tau_exact sorts every switch point instead, as the
reference the histogram is measured against.

device_pick runs the device's own pick (render/lod/r3d_lod.c, with its
hysteresis) over a baked mesh's level arrays, built with the host compiler
through pick_host.c.
"""

import ctypes
import pathlib
from typing import NamedTuple

import numpy as np

from build import packages
from build.c_header import read_define
from native.shared_library import build_shared, find_compiler
from r3d import log
from r3d.cost_model import clusters_in_view
from r3d.mesh_asset import CLUSTER_LOD, LOD_GROUP

HERE = pathlib.Path(__file__).resolve().parent
MAIN = HERE.parents[2] / "main"
LOD_HEADER = MAIN / "render" / "lod" / "r3d_lod.h"
PICK_HOST = HERE / "pick_host.c"
TOLERANCE_PX = read_define(LOD_HEADER, "R3D_LOD_TOLERANCE_PX")
HYSTERESIS_PX = read_define(LOD_HEADER, "R3D_LOD_HYSTERESIS_PX")
WORD_BITS = read_define(LOD_HEADER, "R3D_LOD_WORD_BITS")
COVERAGE_MAX = read_define(LOD_HEADER, "R3D_LOD_COVERAGE_PERMILLE") / read_define(LOD_HEADER, "R3D_LOD_PERMILLE")

# Below half a pixel a coarser level saves too little to be worth its pop; this tree's choice.
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


def placed_boxes(boxes, placed):
    """Boxes (N x 2 x 3, lo and hi) of `placed`'s local mesh as world boxes holding their placed corners."""
    pick_corner = np.array([[i >> axis & 1 for axis in range(3)] for i in range(8)], dtype=bool)
    corners = np.where(pick_corner[None], boxes[:, 1:2], boxes[:, 0:1]) @ placed.matrix.T + placed.position
    return np.stack([corners.min(axis=1), corners.max(axis=1)], axis=1)


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
    parts = []
    for placed in placements:
        levels = placed.levels
        shown = clusters_in_view(placed_boxes(levels.cluster_box, placed), clip)
        own, scale = placed_spheres(levels.own_sphere[shown], placed)
        parent, _ = placed_spheres(levels.parent_sphere[shown], placed)
        parts.append(View(levels.cluster_triangles[shown],
                          projected_px(own, levels.own_error[shown] * scale, eye, focal, near),
                          projected_px(parent, levels.parent_error[shown] * scale, eye, focal, near),
                          levels.cluster_refined[shown] < 0))
    return View(*(np.concatenate(column) for column in zip(*parts)))


def drawn(v, tau):
    """Whether each in-view cluster is drawn at `tau`; a terminal group's
    clusters stay drawn at any tau."""
    return (v.own_px <= tau) & ((tau < v.group_px) | np.isinf(v.group_px))


def triangles_at(v, tau):
    return int(v.triangles[drawn(v, tau)].sum())


def tau_exact(v, budget):
    """The least tau at or above the floor drawing at most `budget` triangles,
    or inf when no tau does. The count can rise with tau where a coarser
    cluster's box reaches into the view and its finer clusters' do not, so
    every switch point above the floor is tried, not only the first."""
    points = np.concatenate([v.own_px, v.group_px])
    steps = np.concatenate([v.triangles, -v.triangles])
    finite = np.isfinite(points)
    points, steps = points[finite], steps[finite]
    order = np.argsort(points, kind="stable")
    points, steps = points[order], np.cumsum(steps[order])
    # The count once tau reaches each distinct switch point.
    last = np.r_[points[1:] != points[:-1], True] if len(points) else np.zeros(0, dtype=bool)
    points, totals = points[last], steps[last]
    above = points > TAU_FLOOR_PX
    candidates = np.r_[TAU_FLOOR_PX, points[above]]
    counts = np.r_[triangles_at(v, TAU_FLOOR_PX), totals[above]]
    meets = np.flatnonzero(counts <= budget)
    return float(candidates[meets[0]]) if len(meets) else np.inf


def bin_edges():
    return TAU_FLOOR_PX * (TAU_CEILING_PX / TAU_FLOOR_PX) ** (np.arange(TAU_BINS + 1) / TAU_BINS)


def tau_from_histogram(v, budget):
    """The least bin edge drawing at most `budget` triangles; the ceiling
    when none does (the budget's safety stop). Where the count only falls as
    tau rises it is within one bin of tau_exact; where culling makes it rise,
    the bins can skip a dip between two edges, so it can land later."""
    edges = bin_edges()
    totals = np.zeros(TAU_BINS + 2, dtype=np.int64)
    # A cluster draws from the first edge at or above its own error until the first edge at or above its group's.
    np.add.at(totals, np.searchsorted(edges, v.own_px, side="left"), v.triangles)
    np.add.at(totals, np.searchsorted(edges, v.group_px, side="left"), -v.triangles)
    totals = np.cumsum(totals)[:TAU_BINS + 1]
    meets = np.flatnonzero(totals <= budget)
    return float(edges[meets[0]] if len(meets) else edges[-1])


_device = None


def _device_library():
    global _device
    if _device is None:
        render = MAIN / "render"
        _device = build_shared("lod_pick", HERE / ".cache", (PICK_HOST, render / "lod" / "r3d_lod.c"),
                               find_compiler("CC", ("cc", "gcc", "clang"), "C"),
                               flags=[f"-I{root}" for root in packages.include_roots()],
                               depends=(LOD_HEADER, render / "r3d_lit_mesh.h", render / "r3d_pipeline.h"),
                               libraries=["-lm"], log=log)
        _device.pick_host.restype = None
    return _device


def device_pick(lod, groups, rows, focal, near, height, tolerance_px=TOLERANCE_PX, coverage_max=COVERAGE_MAX,
                coarse=None):
    """(coarse, drawn): the device's pick over a read mesh's `lod` pairs and
    `groups` rows (lit_mesh.read_lit_mesh) seen through the lens matrix `rows`
    (4 x 4, position ticks to lens space, focal lengths applied to its first
    two rows), from the bits `coarse` of the frame before, all clear when
    None. `focal` is (x, y) in pixels; `height` is the picture's, which
    `coverage_max` is a share of."""
    words = (len(groups) + WORD_BITS - 1) // WORD_BITS
    coarse = np.zeros(words, dtype=np.uint32) if coarse is None else np.array(coarse, dtype=np.uint32)
    pairs = b"".join(CLUSTER_LOD.pack(*pair) for pair in lod)
    records = b"".join(LOD_GROUP.pack(*(int(v) for v in row)) for row in groups)
    lens = np.ascontiguousarray(rows, dtype=np.float32)
    drawn = np.zeros(len(lod), dtype=np.uint8)
    _device_library().pick_host(pairs, ctypes.c_int(len(lod)), records, ctypes.c_int(len(groups)),
                                lens.ctypes.data_as(ctypes.c_void_p), ctypes.c_float(focal[0]),
                                ctypes.c_float(focal[1]), ctypes.c_float(near), ctypes.c_int(height),
                                ctypes.c_float(tolerance_px), ctypes.c_float(coverage_max),
                                coarse.ctypes.data_as(ctypes.c_void_p), drawn.ctypes.data_as(ctypes.c_void_p))
    return coarse, drawn.astype(bool)

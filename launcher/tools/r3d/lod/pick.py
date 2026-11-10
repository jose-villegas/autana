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
from render.settings import render_asset

HERE = pathlib.Path(__file__).resolve().parent
MAIN = HERE.parents[2] / "main"
LOD_HEADER = MAIN / "render" / "lod" / "r3d_lod.h"
PICK_HOST = HERE / "pick_host.c"
SETTINGS = render_asset.load(render_asset.DEFAULT)
TOLERANCE_PX = SETTINGS["lod.tolerance_px"]
HYSTERESIS_PX = SETTINGS["lod.hysteresis_px"]
COVERAGE_PERMILLE = SETTINGS["lod.coverage_permille"]
DROP_PERMILLE_MAX = read_define(MAIN / "render" / "r3d_instance.h", "R3D_LOD_DROP_PERMILLE_MAX")
PERMILLE = read_define(LOD_HEADER, "R3D_LOD_PERMILLE")
# The most a tunable holds: no coverage cap.
UNCAPPED = 2**31 - 1
WORD_BITS = read_define(LOD_HEADER, "R3D_LOD_WORD_BITS")

# The device's steer: it never picks below lod.tolerance_px, and its
# histogram's bin edges are half octaves up to 2^R3D_LOD_BIN_MOST_LOG2.
TAU_FLOOR_PX = TOLERANCE_PX
BIN_LEAST_LOG2 = read_define(LOD_HEADER, "R3D_LOD_BIN_LEAST_LOG2")
BIN_MOST_LOG2 = read_define(LOD_HEADER, "R3D_LOD_BIN_MOST_LOG2")
BINS_PER_OCTAVE = read_define(LOD_HEADER, "R3D_LOD_BINS_PER_OCTAVE")
TAU_CEILING_PX = 2.0 ** BIN_MOST_LOG2


class Placed(NamedTuple):
    """One mesh's levels under one transform: world = matrix @ local + position."""
    levels: object
    matrix: np.ndarray
    position: np.ndarray
    names: tuple = ()


class View(NamedTuple):
    """Every in-view cluster of one pose, plus droppable placements' root errors (NaN outside the view)."""
    triangles: np.ndarray  # per cluster
    own_px: np.ndarray     # its own error, projected; 0 on the finest level
    group_px: np.ndarray   # its group's, projected; terminal groups use the drop error or inf
    finest: np.ndarray     # whether it is a finest-level cluster
    drop_px: tuple = ()    # one projected root radius per droppable placement


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


def view(placements, eye, clip, focal, near, drops=None, height=None):
    """The in-view clusters of every placed mesh at one pose."""
    parts, drop_px = [], []
    for index, placed in enumerate(placements):
        levels = placed.levels
        shown = clusters_in_view(placed_boxes(levels.cluster_box, placed), clip)
        own, scale = placed_spheres(levels.own_sphere[shown], placed)
        parent, _ = placed_spheres(levels.parent_sphere[shown], placed)
        group = projected_px(parent, levels.parent_error[shown] * scale, eye, focal, near)
        if drops and drops.get(index, 0):
            lo, hi = levels.cluster_box[:, 0].min(0), levels.cluster_box[:, 1].max(0)
            root, _ = placed_spheres(np.r_[(lo + hi) / 2, np.linalg.norm(hi - lo) / 2][None], placed)
            empty = projected_px(root, root[:, 3], eye, focal, near)[0]
            picture_height = height if height is not None else 2 * focal / np.linalg.norm(clip[1, :3])
            limit = drops[index] / PERMILLE * picture_height
            empty = empty if empty <= limit else np.inf
            group = np.where(np.isinf(group), empty, group)
            drop_px.append(empty if shown.any() else np.nan)
        parts.append(View(levels.cluster_triangles[shown],
                          projected_px(own, levels.own_error[shown] * scale, eye, focal, near),
                          group,
                          levels.cluster_refined[shown] < 0))
    return View(*(np.concatenate(column) for column in zip(*(part[:4] for part in parts))), tuple(drop_px))


def drawn(v, tau):
    """Whether each in-view cluster is drawn at `tau`; a terminal group's
    clusters stay drawn until their placement drops."""
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
    """The tolerances the device's steer takes: the floor, then every bin edge past it."""
    edges = 2.0 ** (BIN_LEAST_LOG2 + np.arange((BIN_MOST_LOG2 - BIN_LEAST_LOG2) * BINS_PER_OCTAVE + 1) / BINS_PER_OCTAVE)
    return np.r_[TAU_FLOOR_PX, edges[edges > TAU_FLOOR_PX]]


def tau_from_histogram(v, budget):
    """The least bin edge drawing at most `budget` triangles; the ceiling
    when none does (the budget's safety stop). Where the count only falls as
    tau rises it is within one bin of tau_exact; where culling makes it rise,
    the bins can skip a dip between two edges, so it can land later."""
    edges = bin_edges()
    totals = np.zeros(len(edges) + 1, dtype=np.int64)
    # A cluster draws from the first edge at or above its own error until the first edge at or above its group's.
    np.add.at(totals, np.searchsorted(edges, v.own_px, side="left"), v.triangles)
    np.add.at(totals, np.searchsorted(edges, v.group_px, side="left"), -v.triangles)
    totals = np.cumsum(totals)[:len(edges)]
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
                               depends=(LOD_HEADER, render / "r3d_lit_mesh.h", render / "r3d_pipeline.h",
                                        render / "r3d_instance.h", render_asset.HEADER),
                               libraries=["-lm"], log=log)
        _device.pick_host.restype = None
        _device.pick_host_drop_pixels.restype = ctypes.c_float
    return _device


def device_pick(lod, groups, rows, focal, near, height, tolerance_px=TOLERANCE_PX, hysteresis_px=HYSTERESIS_PX,
                coverage_permille=COVERAGE_PERMILLE, coarse=None, drop_permille=0, root=None):
    """(coarse, drawn): the device's pick over a read mesh's `lod` pairs and
    `groups` rows (lit_mesh.read_lit_mesh) seen through the lens matrix `rows`
    (4 x 4, position ticks to lens space, focal lengths applied to its first
    two rows), from the bits `coarse` of the frame before, all clear when
    None. `focal` is (x, y) in pixels; `height` is the picture's, which
    `coverage_permille` is thousandths of. With `drop_permille`, `root` is its local
    tick box (2 x 3); a dropped placement returns every drawn flag clear."""
    if drop_permille and root is None:
        raise ValueError("a placement with a drop limit needs its root box")
    bound = np.ascontiguousarray(root if root is not None else np.zeros((2, 3)), dtype=np.int16)
    if bound.shape != (2, 3):
        raise ValueError("root must be a 2 x 3 tick box")
    words = _device_library().pick_host_words(ctypes.c_int(len(groups)), ctypes.c_int(drop_permille))
    coarse = np.zeros(words, dtype=np.uint32) if coarse is None else np.array(coarse, dtype=np.uint32)
    if coarse.shape != (words,):
        raise ValueError("coarse words must match the placement including its dropped word")
    pairs = b"".join(CLUSTER_LOD.pack(*pair) for pair in lod)
    records = b"".join(LOD_GROUP.pack(*(int(v) for v in row)) for row in groups)
    lens = np.ascontiguousarray(rows, dtype=np.float32)
    drawn = np.zeros(len(lod), dtype=np.uint8)
    _device_library().pick_host(pairs, ctypes.c_int(len(lod)), records, ctypes.c_int(len(groups)),
                                bound.ctypes.data_as(ctypes.c_void_p), ctypes.c_int(drop_permille),
                                lens.ctypes.data_as(ctypes.c_void_p), ctypes.c_float(focal[0]),
                                ctypes.c_float(focal[1]), ctypes.c_float(near), ctypes.c_int(height),
                                ctypes.c_float(tolerance_px), ctypes.c_float(hysteresis_px),
                                ctypes.c_int(coverage_permille),
                                coarse.ctypes.data_as(ctypes.c_void_p), drawn.ctypes.data_as(ctypes.c_void_p))
    return coarse, drawn.astype(bool)

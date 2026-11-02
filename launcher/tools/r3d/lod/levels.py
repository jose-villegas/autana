"""A mesh's cluster level hierarchy (meshoptimizer's clusterlod, through
lod/clusterlod.cpp): the finest clusters, then coarser ones simplified from
groups of them with each group's border locked, until a group no longer
simplifies. Input is welded and position-only, so the simplifier sees one
surface rather than a strip per colour.

The library is meshoptimizer's sources and lod/clusterlod.cpp, built into
.cache/ beside meshopt.py's own, which mesh bakes key on and this does not.

A cluster is drawn while its own error is within the tolerance and its
group's is not (pick.py). Every cluster of a group shares that group's error
and sphere, so neighbours change level together and leave no crack.
"""

import ctypes
import pathlib
from typing import NamedTuple

import numpy as np

from native.shared_library import build_shared, find_compiler
from r3d import log
from r3d.meshopt import CACHE, SOURCE

# clusterlod takes 4..256 triangles a cluster.
CLUSTER_TRIANGLES_MIN = 4
CLUSTER_TRIANGLES_MAX = 256


_lib = None


def _library():
    global _lib
    if _lib is None:
        demo = SOURCE.parent / "demo"
        _lib = build_shared("clusterlod", CACHE, [*sorted(SOURCE.glob("*.cpp")), pathlib.Path(__file__).with_name("clusterlod.cpp")],
                            find_compiler("CXX", ("c++", "g++", "clang++"), "C++"), flags=(f"-I{SOURCE}", f"-I{demo}"),
                            depends=(demo / "clusterlod.h",), log=log)
    return _lib


class Levels(NamedTuple):
    """Every level of one mesh. Spheres are centre and radius, errors are in the mesh's units."""
    positions: np.ndarray       # V x 3, the welded input, shared by every level
    indices: np.ndarray         # 3 per triangle, every cluster's after the other
    cluster_first: np.ndarray   # C: first index of each cluster
    cluster_triangles: np.ndarray  # C
    cluster_sphere: np.ndarray  # C x 4, for the frustum
    cluster_refined: np.ndarray  # C: the group simplified into it, or -1 on the finest level
    cluster_group: np.ndarray   # C: the group it belongs to
    group_sphere: np.ndarray    # G x 4
    group_error: np.ndarray     # G: the error of simplifying it; inf on a terminal group
    group_depth: np.ndarray     # G: 0 for groups of finest clusters


def welded(positions, triangles):
    """`positions` with equal rows joined and `triangles` reindexed to them."""
    points, inverse = np.unique(np.asarray(positions), axis=0, return_inverse=True)
    return points, inverse.reshape(-1)[np.asarray(triangles)]


def build_levels(positions, triangles, cluster_triangles):
    """The levels of a welded mesh (V x 3 positions, T x 3 triangles) with
    clusters of at most `cluster_triangles`."""
    lib = _library()
    size = int(np.clip(cluster_triangles, CLUSTER_TRIANGLES_MIN, CLUSTER_TRIANGLES_MAX))
    p = np.ascontiguousarray(positions, dtype=np.float32)
    i = np.ascontiguousarray(triangles, dtype=np.uint32).reshape(-1)
    c = ctypes.c_void_p
    lib.lod_build.restype = c
    handle = lib.lod_build(p.ctypes.data_as(c), ctypes.c_size_t(len(p)), i.ctypes.data_as(c), ctypes.c_size_t(len(i)),
                           ctypes.c_size_t(size))
    try:
        counts = [ctypes.c_size_t() for _ in range(3)]
        lib.lod_sizes(c(handle), *(ctypes.byref(n) for n in counts))
        clusters, index_count, groups = (n.value for n in counts)
        out = dict(indices=np.empty(index_count, np.uint32), cluster_first=np.empty(clusters, np.uint32),
                   cluster_sphere=np.empty((clusters, 4), np.float32), cluster_refined=np.empty(clusters, np.int32),
                   cluster_group=np.empty(clusters, np.int32), group_sphere=np.empty((groups, 4), np.float32),
                   group_error=np.empty(groups, np.float32), group_depth=np.empty(groups, np.int32))
        lib.lod_read(c(handle), *(array.ctypes.data_as(c) for array in out.values()))
    finally:
        lib.lod_free(c(handle))
    first = out["cluster_first"].astype(np.int64)
    count = np.diff(np.append(first, index_count)) // 3
    error = out["group_error"].astype(np.float64)
    error[error >= np.finfo(np.float32).max] = np.inf
    return Levels(positions=p.astype(np.float64), indices=out["indices"].astype(np.int64), cluster_first=first,
                  cluster_triangles=count, cluster_sphere=out["cluster_sphere"].astype(np.float64),
                  cluster_refined=out["cluster_refined"].astype(np.int64),
                  cluster_group=out["cluster_group"].astype(np.int64),
                  group_sphere=out["group_sphere"].astype(np.float64), group_error=error,
                  group_depth=out["group_depth"].astype(np.int64))

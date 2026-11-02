"""A mesh's cluster level hierarchy (meshoptimizer's clusterlod, through
lod/clusterlod.cpp): the finest clusters, then coarser ones simplified from
groups of them with each group's border locked, until a group no longer
simplifies. Input is welded and position-only, so the simplifier sees one
surface rather than a strip per colour.

The library is meshoptimizer's sources and lod/clusterlod.cpp, built into
.cache/ beside meshopt.py's own; a mesh baked with levels keys on both.

pick.py picks among the levels.
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
# meshoptimizer.h's meshopt_SimplifyVertex_Lock.
SIMPLIFY_VERTEX_LOCK = 1
CLUSTERLOD = pathlib.Path(__file__).with_name("clusterlod.cpp")


_lib = None


def _library():
    global _lib
    if _lib is None:
        demo = SOURCE.parent / "demo"
        if not (demo / "clusterlod.h").exists():
            raise SystemExit(f"{demo} is missing: run `git submodule update --init third_party/upstream/meshoptimizer`")
        _lib = build_shared("clusterlod", CACHE, [*sorted(SOURCE.glob("*.cpp")), CLUSTERLOD],
                            find_compiler("CXX", ("c++", "g++", "clang++"), "C++"), flags=(f"-I{SOURCE}", f"-I{demo}"),
                            depends=(demo / "clusterlod.h",), log=log)
    return _lib


class Levels(NamedTuple):
    """Every level of one mesh, one record per cluster: what the pick reads. Spheres are centre and radius,
    errors are in the mesh's units. A cluster's own sphere and error are its source group's, so they equal
    that group's parent record in every cluster simplified from it, and both sides of a switch agree."""
    positions: np.ndarray       # V x 3, the welded input, shared by every level
    indices: np.ndarray         # 3 per triangle, every cluster's after the other
    cluster_first: np.ndarray   # C: first index of each cluster
    cluster_triangles: np.ndarray  # C
    cluster_box: np.ndarray     # C x 2 x 3: lo and hi of its vertices, for the frustum
    own_sphere: np.ndarray      # C x 4: the group simplified into it; its bounds on the finest level
    own_error: np.ndarray       # C: that group's error, 0 on the finest level
    parent_sphere: np.ndarray   # C x 4: the group it belongs to
    parent_error: np.ndarray    # C: that group's error of simplifying; inf on a terminal group
    cluster_refined: np.ndarray  # C: the group simplified into it, or -1 on the finest level
    cluster_group: np.ndarray   # C: the group it belongs to


def build_levels(positions, triangles, cluster_triangles, locked=None):
    """The levels of a welded mesh (V x 3 positions, T x 3 triangles) with
    clusters of at most `cluster_triangles`; `locked`, one flag per vertex,
    keeps those vertices at every level."""
    lib = _library()
    size = int(np.clip(cluster_triangles, CLUSTER_TRIANGLES_MIN, CLUSTER_TRIANGLES_MAX))
    p = np.ascontiguousarray(positions, dtype=np.float32)
    i = np.ascontiguousarray(triangles, dtype=np.uint32).reshape(-1)
    c = ctypes.c_void_p
    locks = None if locked is None else np.ascontiguousarray(np.where(locked, SIMPLIFY_VERTEX_LOCK, 0), dtype=np.uint8)
    lib.lod_build.restype = c
    handle = lib.lod_build(p.ctypes.data_as(c), ctypes.c_size_t(len(p)), i.ctypes.data_as(c), ctypes.c_size_t(len(i)),
                           ctypes.c_size_t(size), None if locks is None else locks.ctypes.data_as(c))
    try:
        counts = [ctypes.c_size_t() for _ in range(3)]
        lib.lod_sizes(c(handle), *(ctypes.byref(n) for n in counts))
        clusters, index_count, groups = (n.value for n in counts)
        out = dict(indices=np.empty(index_count, np.uint32), cluster_first=np.empty(clusters, np.uint32),
                   cluster_sphere=np.empty((clusters, 4), np.float32), cluster_refined=np.empty(clusters, np.int32),
                   cluster_group=np.empty(clusters, np.int32), group_sphere=np.empty((groups, 4), np.float32),
                   group_error=np.empty(groups, np.float32))
        lib.lod_read(c(handle), *(array.ctypes.data_as(c) for array in out.values()))
    finally:
        lib.lod_free(c(handle))
    positions = p.astype(np.float64)
    indices = out["indices"].astype(np.int64)
    first = out["cluster_first"].astype(np.int64)
    corners = positions[indices]
    box = np.stack([np.minimum.reduceat(corners, first), np.maximum.reduceat(corners, first)], axis=1)
    error = out["group_error"].astype(np.float64)
    error[error >= np.finfo(np.float32).max] = np.inf
    spheres = out["group_sphere"].astype(np.float64)
    refined, group = out["cluster_refined"].astype(np.int64), out["cluster_group"].astype(np.int64)
    finest = refined < 0
    source = np.maximum(refined, 0)
    return Levels(positions=positions, indices=indices, cluster_first=first,
                  cluster_triangles=np.diff(np.append(first, index_count)) // 3, cluster_box=box,
                  own_sphere=np.where(finest[:, None], out["cluster_sphere"].astype(np.float64), spheres[source]),
                  own_error=np.where(finest, 0.0, error[source]), parent_sphere=spheres[group],
                  parent_error=error[group], cluster_refined=refined, cluster_group=group)

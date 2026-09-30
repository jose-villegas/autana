"""meshoptimizer's simplifier and its cluster LOD example, called through
ctypes. The library is the pinned submodule third_party/upstream/meshoptimizer
plus meshopt_lod.cpp, compiled once into .cache/ with the host C++ compiler
(CXX, else c++ or g++)."""

import ctypes
import os
import pathlib
import shutil
import subprocess
import sys

import numpy as np

from . import log

HERE = pathlib.Path(__file__).resolve().parent
UPSTREAM = HERE.parents[2] / "third_party" / "upstream" / "meshoptimizer"
SOURCE = UPSTREAM / "src"
CLUSTERLOD = UPSTREAM / "demo"
SHIM = HERE / "meshopt_lod.cpp"
CACHE = HERE / ".cache"

REGULARIZE = 1 << 4
PERMISSIVE = 1 << 5

_lib = None


def _compiler():
    for name in (os.environ.get("CXX"), "c++", "g++", "clang++"):
        if name and shutil.which(name):
            return shutil.which(name)
    raise SystemExit("no C++ compiler found to build meshoptimizer: set CXX")


def _library():
    global _lib
    if _lib is not None:
        return _lib
    if not (SOURCE / "meshoptimizer.h").exists():
        raise SystemExit(f"{SOURCE} is missing: run `git submodule update --init third_party/upstream/meshoptimizer`")
    CACHE.mkdir(exist_ok=True)
    suffix = {"win32": ".dll"}.get(sys.platform, ".so")
    path = CACHE / f"meshopt{suffix}"
    sources = sorted(SOURCE.glob("*.cpp")) + [SHIM]
    newest = max(f.stat().st_mtime for f in [*sources, CLUSTERLOD / "clusterlod.h"])
    if not path.exists() or path.stat().st_mtime < newest:
        cxx = _compiler()
        log(f"building {path.name} with {cxx}")
        export = "-DMESHOPTIMIZER_API=__declspec(dllexport)" if sys.platform == "win32" else "-fPIC"
        extra = ["-static"] if sys.platform == "win32" else []
        subprocess.run([cxx, "-O2", "-shared", export, *extra, f"-I{SOURCE}", f"-I{CLUSTERLOD}", "-o", str(path),
                        *map(str, sources)], check=True)
    _lib = ctypes.CDLL(str(path))
    return _lib


def simplify_with_update(pos, rgb, tris, target_triangles, colour_weight=1.0, options=REGULARIZE | PERMISSIVE):
    """Simplifies to about target_triangles, keeping colour (0..255 per
    channel) as an attribute and moving the surviving vertices and colours to
    where they best preserve the appearance. Returns (pos, rgb, tris, kept):
    kept[i] is the input vertex output vertex i came from."""
    lib = _library()
    fn = lib.meshopt_simplifyWithUpdate
    fn.restype = ctypes.c_size_t
    p = np.ascontiguousarray(pos, dtype=np.float32).copy()
    a = np.ascontiguousarray(np.asarray(rgb, dtype=np.float64) / 255.0, dtype=np.float32).copy()
    i = np.ascontiguousarray(np.asarray(tris).reshape(-1), dtype=np.uint32).copy()
    w = np.full(3, colour_weight, dtype=np.float32)
    err = ctypes.c_float(0)
    c = ctypes.c_void_p
    n = fn(i.ctypes.data_as(c), ctypes.c_size_t(len(i)), p.ctypes.data_as(c), ctypes.c_size_t(len(p)),
           ctypes.c_size_t(12), a.ctypes.data_as(c), ctypes.c_size_t(12), w.ctypes.data_as(c), ctypes.c_size_t(3),
           None, ctypes.c_size_t(3 * target_triangles), ctypes.c_float(1e9), ctypes.c_uint(options), ctypes.byref(err))
    out = i[:n].reshape(-1, 3).astype(np.int64)
    kept, local = np.unique(out, return_inverse=True)
    return (p[kept].astype(np.float64), np.clip(a[kept] * 255.0, 0.0, 255.0), local.reshape(-1, 3), kept)


class ClusterLod:
    """What meshoptimizer's clusterlod example built from one mesh: groups
    (depth, and the bounds and error each simplified to) and clusters (the
    group each is an input of, the group that produced it, its bounds, a
    normal cone and its triangles as indices into the input vertices)."""

    def __init__(self, group_depth, group_bounds, cluster_group, cluster_refined, cluster_bounds, cluster_cone,
                 cluster_tris):
        self.group_depth = group_depth
        self.group_bounds = group_bounds
        self.cluster_group = cluster_group
        self.cluster_refined = cluster_refined
        self.cluster_bounds = cluster_bounds
        self.cluster_cone = cluster_cone
        self.cluster_tris = cluster_tris


def cluster_lod(pos, rgb, tris, locks=None, max_triangles=64, partition_size=8, colour_weight=1.0):
    """Clusters the triangles into meshlets of at most max_triangles, then
    merges neighbouring meshlets into groups of about partition_size,
    simplifies each group with its outer border locked (colour, 0..255 per
    channel, is a simplification attribute; positions never move) and
    re-splits, until one meshlet is left. `locks` is one byte per vertex,
    non-zero pins the vertex."""
    lib = _library()
    c = ctypes.c_void_p
    p = np.ascontiguousarray(pos, dtype=np.float32).copy()
    a = np.ascontiguousarray(np.asarray(rgb, dtype=np.float64) / 255.0, dtype=np.float32).copy()
    i = np.ascontiguousarray(np.asarray(tris).reshape(-1), dtype=np.uint32).copy()
    lock = np.zeros(len(p), dtype=np.uint8) if locks is None else np.ascontiguousarray(locks, dtype=np.uint8).copy()
    lib.r3d_lod_build.restype = c
    handle = c(lib.r3d_lod_build(p.ctypes.data_as(c), a.ctypes.data_as(c), ctypes.c_size_t(len(p)),
                                 i.ctypes.data_as(c), ctypes.c_size_t(len(i)), lock.ctypes.data_as(c),
                                 ctypes.c_size_t(max_triangles), ctypes.c_size_t(partition_size),
                                 ctypes.c_float(colour_weight)))
    try:
        lib.r3d_lod_group_count.restype = ctypes.c_size_t
        lib.r3d_lod_cluster_count.restype = ctypes.c_size_t
        lib.r3d_lod_cluster.restype = ctypes.c_size_t
        groups, clusters = lib.r3d_lod_group_count(handle), lib.r3d_lod_cluster_count(handle)
        depth = np.zeros(groups, dtype=np.int64)
        gbounds = np.zeros((groups, 5), dtype=np.float32)
        for g in range(groups):
            d = ctypes.c_int()
            lib.r3d_lod_group(handle, ctypes.c_size_t(g), ctypes.byref(d), gbounds[g].ctypes.data_as(c))
            depth[g] = d.value
        cgroup = np.zeros(clusters, dtype=np.int64)
        crefined = np.zeros(clusters, dtype=np.int64)
        cbounds = np.zeros((clusters, 5), dtype=np.float32)
        ccone = np.zeros((clusters, 4), dtype=np.float32)
        ctris = []
        for k in range(clusters):
            g, r = ctypes.c_int(), ctypes.c_int()
            n = lib.r3d_lod_cluster(handle, ctypes.c_size_t(k), ctypes.byref(g), ctypes.byref(r),
                                    cbounds[k].ctypes.data_as(c), ccone[k].ctypes.data_as(c))
            out = np.zeros(n, dtype=np.uint32)
            lib.r3d_lod_cluster_indices(handle, ctypes.c_size_t(k), out.ctypes.data_as(c))
            cgroup[k], crefined[k] = g.value, r.value
            ctris.append(out.reshape(-1, 3).astype(np.int64))
    finally:
        lib.r3d_lod_free(handle)
    return ClusterLod(depth, gbounds, cgroup, crefined, cbounds, ccone, ctris)

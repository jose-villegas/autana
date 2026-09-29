"""meshoptimizer's simplifier, called through ctypes. The library is the
pinned submodule third_party/upstream/meshoptimizer, compiled once into
.cache/ with the host C++ compiler (CXX, else c++ or g++)."""

import ctypes
import os
import pathlib
import shutil
import subprocess
import sys

import numpy as np

from . import log

HERE = pathlib.Path(__file__).resolve().parent
SOURCE = HERE.parents[2] / "third_party" / "upstream" / "meshoptimizer" / "src"
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
    sources = sorted(SOURCE.glob("*.cpp"))
    if not path.exists() or path.stat().st_mtime < max(s.stat().st_mtime for s in sources):
        cxx = _compiler()
        log(f"building {path.name} with {cxx}")
        export = "-DMESHOPTIMIZER_API=__declspec(dllexport)" if sys.platform == "win32" else "-fPIC"
        extra = ["-static"] if sys.platform == "win32" else []
        subprocess.run([cxx, "-O2", "-shared", export, *extra, "-o", str(path), *map(str, sources)], check=True)
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

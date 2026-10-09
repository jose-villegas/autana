"""A shared library built once from source with the host compiler and loaded
through ctypes. It lands in the caller's .cache/ and is rebuilt when a source
or header is newer than it."""

import ctypes
import os
import pathlib
import shutil
import subprocess
import sys


def find_compiler(variable, names, language):
    """The compiler named by the environment variable `variable`, else the
    first of `names` on PATH."""
    for name in (os.environ.get(variable), *names):
        if name and shutil.which(name):
            return shutil.which(name)
    raise SystemExit(f"no {language} compiler found: set {variable}")


def build_shared(name, cache, sources, compiler, flags=(), windows_flags=(), depends=(), libraries=(), log=print,
                 mode=ctypes.DEFAULT_MODE):
    """Compiles `sources` into cache/<name>.dll or .so unless that is current
    against them and `depends`, and returns it loaded. `flags` are for every
    platform, `libraries` follow the sources; Windows also gets `windows_flags` and a static runtime, other
    systems position-independent code. `mode` is ctypes.CDLL's. The library is
    written under a name of this process's and renamed into place, so processes
    building it at once never load a half-written file."""
    cache.mkdir(exist_ok=True)
    windows = sys.platform == "win32"
    path = cache / f"{name}{'.dll' if windows else '.so'}"
    watched = [pathlib.Path(p) for p in (*sources, *depends)]
    if not path.exists() or path.stat().st_mtime < max(p.stat().st_mtime for p in watched):
        log(f"building {path.name} with {compiler}")
        platform_flags = ["-static", *windows_flags] if windows else ["-fPIC"]
        building = path.with_suffix(f".{os.getpid()}{path.suffix}")
        subprocess.run([compiler, "-O2", "-shared", *platform_flags, *flags, "-o", str(building), *map(str, sources),
                        *libraries], check=True)
        os.replace(building, path)
    return ctypes.CDLL(str(path), mode=mode)

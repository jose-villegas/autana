#!/usr/bin/env python3
"""The LLVM C library Dr.Jit's LLVM backend loads on Windows, where no package manager brings one.

    python launcher/tools/r3d/llvm_runtime.py install   # fetch the pinned release's LLVM-C.dll into the user cache
    python launcher/tools/r3d/llvm_runtime.py where     # the library this process would name to Dr.Jit

Dr.Jit loads LLVM-C.dll from the DLL search path unless DRJIT_LIBLLVM_PATH names it. `select()` names the cached
copy for this process only, before Mitsuba loads, so every ray-query tool finds it with no persistent variable. A
DRJIT_LIBLLVM_PATH already set wins. Linux and macOS take libLLVM from the system (apt install llvm).
"""
import argparse
import hashlib
import os
import pathlib
import shutil
import sys
import tarfile
import tempfile
import urllib.request

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[3] / "scripts/lib"))
from autana_config import user_cache_dir  # noqa: E402

VERSION = "23.1.5"
ARCHIVE = f"clang+llvm-{VERSION}-x86_64-pc-windows-msvc.tar.zst"
URL = f"https://github.com/llvm/llvm-project/releases/download/llvmorg-{VERSION}/{ARCHIVE}"
ARCHIVE_SHA256 = "b2be33385ddc86e83511417a060de927ae8b09d1f3017cc3cdce5c73cd5029d7"
LIBRARY = "LLVM-C.dll"
ENV = "DRJIT_LIBLLVM_PATH"
# LLVM's release archives are compressed with a 2 GiB window, past zstd's default decoding limit.
WINDOW_LOG_MAX = 31
CHUNK = 1 << 22


def library(root=None):
    return (root or user_cache_dir()) / "llvm" / VERSION / LIBRARY


def select(environ=os.environ, platform=sys.platform, root=None):
    """Names the cached library to Dr.Jit in `environ` on Windows when it is installed and nothing names one yet.
    Returns the path Dr.Jit will load from, or None when it searches on its own."""
    if environ.get(ENV):
        return pathlib.Path(environ[ENV])
    path = library(root)
    if platform != "win32" or not path.is_file():
        return None
    environ[ENV] = str(path)
    return path


def sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as file:
        while block := file.read(CHUNK):
            digest.update(block)
    return digest.hexdigest()


def extract(archive, target):
    """Writes the archive's bin/LLVM-C.dll to `target`; raises when the archive has none."""
    from compression import zstd

    options = {zstd.DecompressionParameter.window_log_max: WINDOW_LOG_MAX}
    with zstd.ZstdFile(archive, options=options) as stream, tarfile.open(fileobj=stream, mode="r|") as tar:
        for member in tar:
            if member.isfile() and member.name.endswith("/bin/" + LIBRARY):
                with tar.extractfile(member) as source, open(target, "wb") as out:
                    shutil.copyfileobj(source, out, CHUNK)
                return
    raise RuntimeError(f"{archive.name} has no bin/{LIBRARY}")


def install(root=None, url=URL, expected=ARCHIVE_SHA256):
    """Fetches the pinned archive, checks its SHA-256 before reading it, and keeps only the library. Returns its path;
    an installed one is kept."""
    path = library(root)
    if path.is_file():
        return path
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=path.parent) as scratch:
        archive = pathlib.Path(scratch) / ARCHIVE
        with urllib.request.urlopen(url) as response, open(archive, "wb") as out:
            shutil.copyfileobj(response, out, CHUNK)
        found = sha256(archive)
        if found != expected:
            raise RuntimeError(f"{ARCHIVE}: SHA-256 {found}, pinned {expected}")
        partial = pathlib.Path(scratch) / LIBRARY
        extract(archive, partial)
        os.replace(partial, path)
    return path


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("command", choices=("install", "where"))
    args = parser.parse_args(argv)
    if args.command == "install":
        print(install())
        return 0
    found = select(dict(os.environ))
    print(found or f"none: Dr.Jit searches the DLL path for {LIBRARY}")
    return 0 if found else 1


if __name__ == "__main__":
    sys.exit(main())

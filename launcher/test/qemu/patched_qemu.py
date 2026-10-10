#!/usr/bin/env python3
"""Espressif's QEMU at the version ESP-IDF installs, rebuilt with the fixes
in patches/ beside this file, for the runs qemu_run.py starts.

    python3 launcher/test/qemu/patched_qemu.py build   # once; Linux
    python3 launcher/test/qemu/patched_qemu.py path    # the binary, or exit 1
    python3 launcher/test/qemu/patched_qemu.py deps    # Debian/Ubuntu packages

Each patch fixes a QEMU bug that fails runs of this firmware and names its
upstream issue in its header; a patch is deleted once the QEMU version
ESP-IDF installs has the fix (it then stops applying, and the build says
which one). The version is read from ESP-IDF's own tools.json, so a newer
ESP-IDF rebuilds against its QEMU with no edit here.

The build installs into launcher/build.qemu-xtensa/<version>-<patch hash>/,
one directory per version and patch set; qemu_run.py runs that binary when
it exists and says so, and otherwise runs Espressif's prebuilt one, which
lacks the patches. The build is Linux only: on Windows, runs keep the
prebuilt QEMU and the races the patches fix.
"""
import hashlib
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
PATCHES = HERE / "patches"
INSTALL_ROOT = HERE.parent.parent / "build.qemu-xtensa"
SOURCE = "https://github.com/espressif/qemu"
TOOL = "qemu-xtensa"
TARGET = "xtensa-softmmu"
PATCH_HASH_CHARS = 12

# What configure needs beyond a C toolchain, as Debian/Ubuntu name it.
# Espressif's own Linux release build enables the same libraries
# (.github/workflows/scripts/configure-native.sh in their repository),
# less SDL: these runs have no window.
DEBIAN_PACKAGES = ("build-essential", "git", "ninja-build", "pkg-config",
                   "python3-venv", "flex", "bison", "libglib2.0-dev",
                   "libpixman-1-dev", "libgcrypt20-dev", "libslirp-dev")


def idf_qemu_version(idf_path):
    """The qemu-xtensa version ESP-IDF's tools.json recommends, such as
    esp_develop_9.2.2_20260417; its release tag spells it with dashes."""
    with open(Path(idf_path) / "tools" / "tools.json", encoding="utf-8") as fh:
        tools = json.load(fh)["tools"]
    tool = next(t for t in tools if t["name"] == TOOL)
    return next(v["name"] for v in tool["versions"] if v.get("status") == "recommended")


def patch_files():
    return sorted(PATCHES.glob("*.patch"))


def patch_hash():
    """Changes with any patch's name or content, so an edited patch set
    builds into a directory of its own."""
    digest = hashlib.sha256()
    for patch in patch_files():
        digest.update(patch.name.encode() + b"\0" + patch.read_bytes() + b"\0")
    return digest.hexdigest()[:PATCH_HASH_CHARS]


def install_dir(idf_path):
    return INSTALL_ROOT / ("%s-%s" % (idf_qemu_version(idf_path), patch_hash()))


def binary(prefix):
    return prefix / "bin" / ("qemu-system-xtensa.exe" if os.name == "nt" else "qemu-system-xtensa")


def installed(idf_path):
    """The patched binary for this ESP-IDF's QEMU version and these
    patches, or None when it has not been built."""
    path = binary(install_dir(idf_path))
    return path if path.is_file() else None


def run(cmd, cwd):
    print("+ " + " ".join(str(c) for c in cmd), flush=True)
    subprocess.run([str(c) for c in cmd], cwd=cwd, check=True)


def build(idf_path):
    if sys.platform != "linux":
        sys.exit("patched_qemu: builds on Linux only; on this host qemu_run.py "
                 "runs Espressif's prebuilt QEMU, without patches/")
    prefix = install_dir(idf_path)
    if binary(prefix).is_file():
        print(binary(prefix))
        return
    version = idf_qemu_version(idf_path)
    work = INSTALL_ROOT / ("src-" + prefix.name)
    shutil.rmtree(work, ignore_errors=True)
    run(["git", "clone", "--quiet", "--depth", "1", "--branch", version.replace("_", "-"),
         SOURCE, work], INSTALL_ROOT)
    for patch in patch_files():
        run(["git", "apply", "--verbose", patch], work)
    run(["./configure", "--prefix=%s" % prefix, "--bindir=bin", "--datadir=share/qemu",
         "--with-suffix=", "--target-list=%s" % TARGET, "--without-default-features",
         "--enable-gcrypt", "--enable-pixman", "--enable-slirp",
         "--with-pkgversion=%s+%s" % (version, patch_hash())], work)
    run(["make", "-j%d" % (os.cpu_count() or 1), "install"], work)
    shutil.rmtree(work)
    # Other versions and patch sets: nothing runs them any more.
    for stale in INSTALL_ROOT.iterdir():
        if stale != prefix:
            shutil.rmtree(stale)
    print(binary(prefix))


def main(argv):
    from espressif import idf_path
    command = argv[0] if argv else ""
    if command == "deps":
        print(" ".join(DEBIAN_PACKAGES))
    elif command == "path":
        path = installed(idf_path())
        if not path:
            return 1
        print(path)
    elif command == "build":
        INSTALL_ROOT.mkdir(exist_ok=True)
        build(idf_path())
    else:
        sys.exit("usage: patched_qemu.py build | path | deps")
    return 0


if __name__ == "__main__":
    sys.path.insert(0, str(HERE.parent.parent / "tools" / "build"))
    sys.exit(main(sys.argv[1:]))

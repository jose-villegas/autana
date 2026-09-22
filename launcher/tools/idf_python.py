"""Finds the ESP-IDF-managed Python interpreter this repo's host tooling
needs (the one with pyserial and esptool installed) - shared by
scripts/device/device.py and launcher/test/qemu_run.py so a version bump
only has to be taught here.

    from idf_python import idf_python
    python = idf_python()  # falls back to sys.executable if IDF isn't found

Windows only: on any other OS, ESP-IDF's own tools are on PATH once its
export script is sourced, and this repo's host tooling has never needed to
hunt for them there.
"""
import glob
import os
import re
import sys

GLOB = "~/.espressif/python_env/idf*_env/Scripts/python.exe"
VERSION = re.compile(r"idf(\d+)\.(\d+)_py(\d+)\.(\d+)_env")


def _version_key(path):
    """(idf major, idf minor, py major, py minor), so idf5.10 sorts after
    idf5.9 and py3.14 after py3.9 - a plain string sort gets both backwards
    (`"5.10" < "5.9"`)."""
    m = VERSION.search(path)
    return tuple(int(g) for g in m.groups()) if m else (0, 0, 0, 0)


def idf_python():
    """`IDF_PYTHON_ENV_PATH` (set once ESP-IDF's export script is sourced)
    first, then the highest-versioned match under `~/.espressif`, then this
    interpreter - the same fallback both former call sites already used."""
    if os.name != "nt":
        return sys.executable
    env = os.environ.get("IDF_PYTHON_ENV_PATH")
    if env:
        candidate = os.path.join(env, "Scripts", "python.exe")
        if os.path.isfile(candidate):
            return candidate
    found = sorted(glob.glob(os.path.expanduser(GLOB)), key=_version_key)
    return found[-1] if found else sys.executable

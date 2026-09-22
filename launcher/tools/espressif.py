"""Where ESP-IDF's tool installer put things, and the one Python interpreter
inside it this repo's host tooling needs (the one with pyserial and esptool
installed) - shared by scripts/device/device.py, launcher/test/qemu_run.py
and launcher/tools/complexity_gate.py so each fact lives in one place.

    from espressif import espressif_tools_root, idf_python
    root = espressif_tools_root()   # $IDF_TOOLS_PATH, or ~/.espressif
    python = idf_python()           # falls back to sys.executable if not found

Windows only: elsewhere ESP-IDF's export script puts its Python on PATH, so
sys.executable is already the right one.
"""
import glob
import os
import re
import sys
from pathlib import Path

VERSION = re.compile(r"idf(\d+)\.(\d+)_py(\d+)\.(\d+)_env")


def espressif_tools_root():
    """Where ESP-IDF's tool installer put its toolchains and python
    virtualenvs: $IDF_TOOLS_PATH when set (ESP-IDF's container images use
    /opt/esp), else the installer's default under the home directory."""
    env = os.environ.get("IDF_TOOLS_PATH")
    return Path(env) if env else Path.home() / ".espressif"


def _version_key(path):
    """(idf major, idf minor, py major, py minor), so idf5.10 sorts after
    idf5.9 and py3.14 after py3.9 - a plain string sort gets both backwards
    (`"5.10" < "5.9"`)."""
    m = VERSION.search(path)
    return tuple(int(g) for g in m.groups()) if m else (0, 0, 0, 0)


def idf_python():
    """`IDF_PYTHON_ENV_PATH` (set once ESP-IDF's export script is sourced)
    first, then the highest-versioned match under espressif_tools_root(),
    then this interpreter."""
    if os.name != "nt":
        return sys.executable
    env = os.environ.get("IDF_PYTHON_ENV_PATH")
    if env:
        candidate = os.path.join(env, "Scripts", "python.exe")
        if os.path.isfile(candidate):
            return candidate
    pattern = str(espressif_tools_root() / "python_env" / "idf*_env" / "Scripts" / "python.exe")
    found = sorted(glob.glob(pattern), key=_version_key)
    return found[-1] if found else sys.executable

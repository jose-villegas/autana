"""Where ESP-IDF's tool installer put things (espressif_tools_root()), the
ESP-IDF checkout itself (idf_path()), and the interpreter inside the tools
that has pyserial and esptool (idf_python()).

    from espressif import espressif_tools_root, idf_path, idf_python
    root = espressif_tools_root()   # $IDF_TOOLS_PATH, or ~/.espressif
    idf = idf_path()                # $IDF_PATH, or ~/esp/esp-idf
    python = idf_python()           # falls back to sys.executable if not found

idf_python() searches only on Windows: elsewhere ESP-IDF's export script
puts its Python on PATH, so sys.executable is already the right one.
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


def idf_path():
    """The ESP-IDF checkout: $IDF_PATH, which ESP-IDF's installers and
    export scripts set, else Espressif's documented ~/esp/esp-idf; the
    same rule as idf.sh's idf_default_export(). It may not exist."""
    env = os.environ.get("IDF_PATH")
    return Path(env) if env else Path.home() / "esp" / "esp-idf"


def _version_key(path):
    """(idf major, idf minor, py major, py minor), so idf5.10 sorts after
    idf5.9 and py3.14 after py3.9; a plain string sort gets both backwards
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

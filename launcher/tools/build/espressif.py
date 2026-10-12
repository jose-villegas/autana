"""Where ESP-IDF's tool installer put things (espressif_tools_root()), the
ESP-IDF checkout itself (idf_path()), the interpreter inside the tools that
has pyserial and esptool (idf_python()), and the ESP-IDF pinned for CI and
for matching locally (idf_pin()).

    from espressif import espressif_tools_root, idf_path, idf_pin, idf_python
    root = espressif_tools_root()   # $IDF_TOOLS_PATH, or ~/.espressif
    idf = idf_path()                # $IDF_PATH, or ~/esp/esp-idf
    python = idf_python()           # falls back to sys.executable if not found
    pin = idf_pin()                 # launcher/idf-pin.json

It also reads the pinned IDF image and commit (idf_pin(), launcher/idf-pin.json)
and names a build's drift from it (idf_drift()).

idf_python() searches only on Windows: elsewhere ESP-IDF's export script
puts its Python on PATH, so sys.executable is already the right one.
"""
import json
import glob
import os
import re
import subprocess
import sys
from pathlib import Path

VERSION = re.compile(r"idf(\d+)\.(\d+)_py(\d+)\.(\d+)_env")
ESP_CLANG = re.compile(r"esp-(\d+\.\d+\.\d+)")


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


IDF_PIN = "launcher/idf-pin.json"


def idf_pin(root=Path(__file__).resolve().parents[3]):
    """launcher/idf-pin.json: the image CI builds in (`image`, its tag and
    digest), that image's ESP-IDF `commit`, and the LLVM release of its
    esp-clang (`esp_clang`), which also pins clang-format and clang-tidy."""
    return json.loads((root / IDF_PIN).read_text(encoding="utf-8"))


def idf_commit(idf):
    """The commit the ESP-IDF checkout at `idf` is at, or None when it is not one."""
    try:
        result = subprocess.run(["git", "-C", str(idf), "rev-parse", "HEAD"], capture_output=True, text=True)
    except OSError:
        return None
    return result.stdout.strip() if result.returncode == 0 else None


def esp_clang(tools_json):
    """The LLVM release of the esp-clang ESP-IDF's tools.json recommends: esp-19.1.2_20250312 is 19.1.2."""
    tools = json.loads(Path(tools_json).read_text(encoding="utf-8"))["tools"]
    tool = next(t for t in tools if t["name"] == "esp-clang")
    name = next(v["name"] for v in tool["versions"] if v.get("status") == "recommended")
    return ESP_CLANG.match(name).group(1)


def idf_facts(idf):
    """What launcher/idf-pin.json records about the ESP-IDF at `idf`."""
    return {"commit": idf_commit(idf), "esp_clang": esp_clang(Path(idf) / "tools" / "tools.json")}


def idf_drift(found, pin=None):
    """One line per fact in `found` the pin records otherwise, naming both values;
    empty when they all match."""
    pin = idf_pin() if pin is None else pin
    return [f"{key} {found[key]} here, {pin.get(key) or '(none)'} pinned"
            for key in found if pin.get(key) != found[key]]


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

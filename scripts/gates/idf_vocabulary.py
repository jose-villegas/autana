"""The names ESP-IDF and its toolchain's C library spell, for a citation of
something this tree uses but does not define.

    from idf_vocabulary import outside_vocabulary
    outside = outside_vocabulary()      # None when no ESP-IDF is installed
    outside.functions, outside.constants, outside.has_path("hal/spi_ll.h")

ESP-IDF is the checkout espressif.idf_path() finds ($IDF_PATH, else
~/esp/esp-idf); the C library is the headers of every xtensa-esp-elf
toolchain under espressif_tools_root(). From ESP-IDF's components/ and
tools/ it takes every file a doc can cite by path, every name C code calls
or declares with `(`, every CONSTANT_NAME C or CMake code spells, and
CONFIG_<NAME> for every option a Kconfig file declares; from the toolchain,
the same from its headers. Comments are stripped first, so a name only a
comment mentions is not vocabulary. examples/ and docs/ are left out: this
tree cites the SDK, not its samples.

Every chip's files are read, not only this tree's target: a doc may say
what another chip has (a capability the ESP32-P4 alone defines). A scan
reads some thirty thousand files, spread across every core, so the result is
cached as JSON under the system temp directory (cache_directory()), keyed by
the checkout's path, its git commit, its version.cmake, the toolchain
directories and this file's own text. An edit inside ESP-IDF that is not
committed does not change the key; delete the cache directory to rescan.
"""
import concurrent.futures
import hashlib
import json
import os
import pathlib
import re
import subprocess
import sys
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "launcher" / "tools" / "build"))
from espressif import espressif_tools_root, idf_path  # noqa: E402  (path must be set up first)

SCANNED = ("components", "tools")
# Files per worker task; a smaller scan is read in-process, cheaper than
# starting a pool.
CHUNK = 500
C_SUFFIXES = {".c", ".h"}
CITABLE_SUFFIXES = {".c", ".h", ".py", ".sh", ".cmake", ".md"}
C_COMMENT = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)
CMAKE_COMMENT = re.compile(r"#[^\n]*")
FUNCTION = re.compile(r"\b([A-Za-z_]\w*)\s*\(")
CONSTANT = re.compile(r"\b([A-Z][A-Z0-9]*_[A-Z0-9_]*[A-Z0-9])\b")
KCONFIG = re.compile(r"^\s*(?:menuconfig|config|choice)\s+([A-Z0-9_]+)\b", re.M)


class OutsideVocabulary:
    """What ESP-IDF and the C library define. `paths` are posix paths
    relative to the checkout or include directory they came from."""

    def __init__(self, source, functions=(), constants=(), paths=()):
        self.source = source
        self.functions = set(functions)
        self.constants = set(constants)
        self.paths = sorted(paths)

    def has_path(self, value):
        """Whether `value`, a bare file name or a trailing run of path
        segments, names a file in the scan."""
        suffix = "/" + value
        return any(("/" + path).endswith(suffix) for path in self.paths)


def toolchain_includes(tools_root=None):
    """Every installed xtensa-esp-elf toolchain's C library headers."""
    root = pathlib.Path(tools_root) if tools_root else espressif_tools_root()
    return sorted(root.glob("tools/xtensa-esp-elf/*/xtensa-esp-elf/xtensa-esp-elf/include"))


def _commit(idf):
    try:
        out = subprocess.run(["git", "-c", f"safe.directory={idf.resolve().as_posix()}",
                              "-C", str(idf), "rev-parse", "HEAD"],
                             capture_output=True, text=True, check=False)
    except OSError:
        return None
    return out.stdout.strip() if out.returncode == 0 else None


def _key(idf, includes):
    version = idf / "tools" / "cmake" / "version.cmake"
    parts = [str(idf.resolve()), _commit(idf),
             version.read_text(encoding="utf-8", errors="replace") if version.is_file() else None,
             [str(p.resolve()) for p in includes],
             pathlib.Path(__file__).read_text(encoding="utf-8")]
    return hashlib.sha256(json.dumps(parts).encode()).hexdigest()[:32]


def _read(path):
    return path.read_text(encoding="utf-8", errors="replace")


def _scan_files(paths):
    functions, constants = set(), set()
    for path in paths:
        if path.suffix in C_SUFFIXES:
            code = C_COMMENT.sub(" ", _read(path))
            functions.update(FUNCTION.findall(code))
            constants.update(CONSTANT.findall(code))
        elif path.suffix == ".cmake" or path.name == "CMakeLists.txt":
            constants.update(CONSTANT.findall(CMAKE_COMMENT.sub(" ", _read(path))))
        elif path.name.startswith("Kconfig"):
            options = KCONFIG.findall(_read(path))
            constants.update(options)
            constants.update("CONFIG_" + name for name in options)
    return functions, constants


def _files(base):
    if base.is_file():
        yield base
        return
    for directory, subdirs, files in os.walk(base):
        subdirs[:] = [d for d in subdirs if d != ".git"]
        for name in files:
            yield pathlib.Path(directory) / name


def scan(idf, includes):
    """The vocabulary of the checkout `idf` and of the header directories
    `includes`, as a dict of sorted lists. The files are read across every
    core: one reader takes minutes."""
    tops = [idf / name for name in SCANNED] + [p for p in idf.iterdir() if p.is_file()]
    files = [path for top in tops for path in _files(top)]
    paths = {path.relative_to(idf).as_posix() for path in files
             if path.suffix in CITABLE_SUFFIXES or path.name == "CMakeLists.txt"}
    for include in includes:
        headers = [path for path in _files(include) if path.suffix == ".h"]
        paths |= {path.relative_to(include).as_posix() for path in headers}
        files += headers
    chunks = [files[i:i + CHUNK] for i in range(0, len(files), CHUNK)]
    functions, constants = set(), set()
    if len(chunks) > 1:
        with concurrent.futures.ProcessPoolExecutor() as pool:
            found = list(pool.map(_scan_files, chunks))
    else:
        found = [_scan_files(files)]
    for found_functions, found_constants in found:
        functions |= found_functions
        constants |= found_constants
    return {"functions": sorted(functions), "constants": sorted(constants), "paths": sorted(paths)}


def cache_directory():
    return pathlib.Path(tempfile.gettempdir()) / "autana-outside-vocabulary"


def outside_vocabulary(idf=None, tools_root=None, cache_dir=None):
    """The OutsideVocabulary of ESP-IDF at `idf` (default idf_path()), or
    None when there is no ESP-IDF there. Scanned once per cache key."""
    idf = pathlib.Path(idf) if idf else idf_path()
    if not (idf / "components").is_dir():
        return None
    includes = toolchain_includes(tools_root)
    cache_dir = pathlib.Path(cache_dir) if cache_dir else cache_directory()
    cache = cache_dir / f"{_key(idf, includes)}.json"
    try:
        data = json.loads(cache.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        data = scan(idf, includes)
        cache_dir.mkdir(parents=True, exist_ok=True)
        partial = cache.with_suffix(f".{os.getpid()}.tmp")
        partial.write_text(json.dumps(data), encoding="utf-8")
        os.replace(partial, cache)
    return OutsideVocabulary(idf, data["functions"], data["constants"], data["paths"])


def no_idf():
    """Why there is no outside vocabulary, for a gate to print."""
    return f"no ESP-IDF at {idf_path()} (set IDF_PATH)"


def not_verified_notice(count):
    """The one line a gate prints when `count` names went unchecked."""
    return (f"{count} cited name{'' if count == 1 else 's'} this tree does not define "
            f"went unchecked: {no_idf()}")

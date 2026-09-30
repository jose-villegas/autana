"""The names ESP-IDF and its toolchain's C library declare, for a citation
of something this tree uses but does not define.

    from idf_vocabulary import outside_vocabulary
    outside = outside_vocabulary()      # None when no ESP-IDF is installed
    outside.functions, outside.constants, outside.has_path("hal/spi_ll.h")

ESP-IDF is the checkout espressif.idf_path() finds ($IDF_PATH, else
~/esp/esp-idf); the C library is the headers of every xtensa-esp-elf
toolchain under espressif_tools_root(). From ESP-IDF's components/ and
tools/ it takes every file a doc can cite by path; what C code declares:
prototypes, definitions, #defines, enum values, type names, with comments
and string literals blanked, so a name ESP-IDF only calls, quotes or
mentions is not vocabulary; every CONSTANT_NAME CMake code spells; and
CONFIG_<NAME> for every Kconfig option. From the toolchain, the same from
its headers. examples/ and docs/ are left out: this tree cites the SDK, not
its samples.

The known limit: every chip's files are read, so a name resolves when
ESP-IDF declares it for some chip, not necessarily for the ESP32-S3. A doc
may say what another chip has (a capability only the ESP32-P4 defines), and
that must resolve too.

A scan reads some thirty thousand files, spread across every core, so the
result is cached as JSON under the system temp directory
(cache_directory()), keyed by every scanned file's path, size and
modification time and this file's own text: a new commit, an uncommitted
edit or a toolchain header rewritten in place is a new key.
"""
import concurrent.futures
import hashlib
import json
import os
import pathlib
import re
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
COMMENT_OR_LITERAL = re.compile(r"""/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\\n])*"|'(?:\\.|[^'\\\n])*'""", re.S)
DIRECTIVE = re.compile(r"^[ \t]*#(?:[^\n]*\\\n)*[^\n]*", re.M)
DEFINE = re.compile(r"^[ \t]*#[ \t]*define[ \t]+([A-Za-z_]\w*)(\()?", re.M)
# A statement that opens with words and ends its last word in `(`: a
# prototype or a definition. A call has no words before it, or a keyword
# (return, else) that FLOW rejects.
DECLARATION = re.compile(r"(?:^|(?<=[;{}]))[ \t]*"
                         r"((?:(?:__attribute__\s*\(\([^;{}]*?\)\)|[A-Za-z_]\w*)[\s*]+)+)"
                         r"([A-Za-z_]\w*)\s*\(", re.M)
FLOW = {"return", "else", "case", "goto", "do", "sizeof", "typedef", "if", "while", "for",
        "switch", "defined"}
NOT_A_NAME = FLOW | {"void", "char", "short", "int", "long", "float", "double", "signed",
                     "unsigned", "const", "volatile", "static", "inline", "extern", "struct",
                     "union", "enum", "__attribute__", "__asm__", "asm", "_Static_assert",
                     "static_assert", "typeof", "__typeof__", "alignof", "_Alignof"}
WORD = re.compile(r"\w+")
ENUM_BODY = re.compile(r"\benum\b[^{};()]*\{([^{}]*)\}")
ENUM_VALUE = re.compile(r"(?:^|,)\s*([A-Za-z_]\w*)")
TAG = re.compile(r"\b(?:struct|union|enum)\s+([A-Za-z_]\w*)\s*\{")
BODY_NAME = re.compile(r"\}\s*([A-Za-z_]\w*)\s*(?:\[[^\]]*\]\s*)*;")
TYPEDEF = re.compile(r"\btypedef\b[^;{}]*?\b([A-Za-z_]\w*)\s*(?:\[[^\]]*\]\s*)*;")
TYPEDEF_POINTER = re.compile(r"\btypedef\b[^;{}]*?\(\s*\*\s*([A-Za-z_]\w*)\s*\)")
CMAKE_COMMENT = re.compile(r"#[^\n]*")
CONSTANT = re.compile(r"\b([A-Z][A-Z0-9]*_[A-Z0-9_]*[A-Z0-9])\b")
KCONFIG = re.compile(r"^\s*(?:menuconfig|config|choice)\s+([A-Z0-9_]+)\b", re.M)


class OutsideVocabulary:
    """What ESP-IDF and the C library declare. `paths` are posix paths
    relative to the checkout or include directory they came from;
    `has_c_library` says whether any toolchain header was found."""

    def __init__(self, source, functions=(), constants=(), types=(), paths=(),
                 has_c_library=False, tools_root=None):
        self.source = source
        self.functions = set(functions)
        self.constants = set(constants)
        self.types = set(types)
        self.paths = sorted(paths)
        self.has_c_library = has_c_library
        self.tools_root = tools_root

    def has_path(self, value):
        """Whether `value`, a bare file name or a trailing run of path
        segments, names a file in the scan."""
        suffix = "/" + value
        return any(("/" + path).endswith(suffix) for path in self.paths)


def toolchain_includes(tools_root=None):
    """Every installed xtensa-esp-elf toolchain's C library headers."""
    root = pathlib.Path(tools_root) if tools_root else espressif_tools_root()
    return sorted(root.glob("tools/xtensa-esp-elf/*/xtensa-esp-elf/xtensa-esp-elf/include"))


def _read(path):
    return path.read_text(encoding="utf-8", errors="replace")


def _blank(match):
    return "\n" * match.group().count("\n") or " "


def c_declarations(text):
    """(functions, constants, types) C source `text` declares: prototypes
    and definitions, #defines (a function-like one is a function too), enum
    values, and struct/union/enum tags and typedef names."""
    code = COMMENT_OR_LITERAL.sub(_blank, text)
    functions, constants, types = set(), set(), set()
    for name, paren in DEFINE.findall(code):
        constants.add(name)
        if paren:
            functions.add(name)
    code = DIRECTIVE.sub(_blank, code)
    for prefix, name in DECLARATION.findall(code):
        if name not in NOT_A_NAME and FLOW.isdisjoint(WORD.findall(prefix)):
            functions.add(name)
    for body in ENUM_BODY.findall(code):
        constants.update(ENUM_VALUE.findall(body))
    for pattern in (TAG, BODY_NAME, TYPEDEF, TYPEDEF_POINTER):
        types.update(pattern.findall(code))
    return functions, constants, types


def _scan_files(paths):
    functions, constants, types = set(), set(), set()
    for path in paths:
        if path.suffix in C_SUFFIXES:
            found_functions, found_constants, found_types = c_declarations(_read(path))
            functions |= found_functions
            constants |= found_constants
            types |= found_types
        elif path.suffix == ".cmake" or path.name == "CMakeLists.txt":
            constants.update(CONSTANT.findall(CMAKE_COMMENT.sub(" ", _read(path))))
        elif path.name.startswith("Kconfig"):
            options = KCONFIG.findall(_read(path))
            constants.update(options)
            constants.update("CONFIG_" + name for name in options)
    return functions, constants, types


def _walk(base, out):
    """Append (path, stat) for every file under `base`, .git left out."""
    with os.scandir(base) as entries:
        for entry in entries:
            if entry.is_dir(follow_symlinks=False):
                if entry.name != ".git":
                    _walk(entry.path, out)
            else:
                out.append((pathlib.Path(entry.path), entry.stat(follow_symlinks=False)))


def _listing(idf, includes):
    """(sources, headers): every ESP-IDF file scanned and every toolchain
    header, each as (path, include directory or None, stat)."""
    sources = [(path, None, path.stat()) for path in idf.iterdir() if path.is_file()]
    for name in SCANNED:
        found = []
        if (idf / name).is_dir():
            _walk(idf / name, found)
        sources += [(path, None, info) for path, info in found]
    headers = []
    for include in includes:
        found = []
        _walk(include, found)
        headers += [(path, include, info) for path, info in found if path.suffix == ".h"]
    return sources, headers


def _key(idf, files):
    digest = hashlib.sha256(str(idf.resolve()).encode())
    for path, _, info in files:
        digest.update(f"{path}\0{info.st_size}\0{info.st_mtime_ns}\n".encode())
    digest.update(pathlib.Path(__file__).read_bytes())
    return digest.hexdigest()[:32]


def scan(idf, files):
    """The vocabulary of `files` (see _listing()) under the checkout `idf`,
    as a dict of sorted lists. The files are read across every core: one
    reader takes minutes."""
    paths = {path.relative_to(include or idf).as_posix() for path, include, _ in files
             if include or path.suffix in CITABLE_SUFFIXES or path.name == "CMakeLists.txt"}
    names = [path for path, _, _ in files]
    chunks = [names[i:i + CHUNK] for i in range(0, len(names), CHUNK)]
    if len(chunks) > 1:
        with concurrent.futures.ProcessPoolExecutor() as pool:
            found = list(pool.map(_scan_files, chunks))
    else:
        found = [_scan_files(names)]
    functions, constants, types = set(), set(), set()
    for found_functions, found_constants, found_types in found:
        functions |= found_functions
        constants |= found_constants
        types |= found_types
    return {"functions": sorted(functions), "constants": sorted(constants),
            "types": sorted(types), "paths": sorted(paths)}


def cache_directory():
    return pathlib.Path(tempfile.gettempdir()) / "autana-outside-vocabulary"


def outside_vocabulary(idf=None, tools_root=None, cache_dir=None):
    """The OutsideVocabulary of ESP-IDF at `idf` (default idf_path()), or
    None when there is no ESP-IDF there. Scanned once per cache key."""
    idf = pathlib.Path(idf) if idf else idf_path()
    if not (idf / "components").is_dir():
        return None
    tools_root = pathlib.Path(tools_root) if tools_root else espressif_tools_root()
    sources, headers = _listing(idf, toolchain_includes(tools_root))
    files = sources + headers
    cache_dir = pathlib.Path(cache_dir) if cache_dir else cache_directory()
    cache = cache_dir / f"{_key(idf, files)}.json"
    try:
        data = json.loads(cache.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        data = scan(idf, files)
        cache_dir.mkdir(parents=True, exist_ok=True)
        partial = cache.with_suffix(f".{os.getpid()}.tmp")
        partial.write_text(json.dumps(data), encoding="utf-8")
        os.replace(partial, cache)
    return OutsideVocabulary(idf, data["functions"], data["constants"], data["types"],
                             data["paths"], bool(headers), tools_root)


def no_idf():
    """Why there is no outside vocabulary, for a gate to print."""
    return f"no ESP-IDF at {idf_path()} (set IDF_PATH)"


def required_missing(outside):
    """Why --require-idf must fail for `outside`, or None: no ESP-IDF, or
    no toolchain C library beside it."""
    if outside is None:
        return no_idf()
    if not outside.has_c_library:
        return (f"no C library headers under {outside.tools_root}/tools/xtensa-esp-elf "
                f"(set IDF_TOOLS_PATH, or install the toolchain with idf_tools.py)")
    return None


def not_verified_notice(count):
    """The one line a gate prints when `count` names went unchecked."""
    return (f"{count} cited name{'' if count == 1 else 's'} this tree does not define "
            f"went unchecked: {no_idf()}")

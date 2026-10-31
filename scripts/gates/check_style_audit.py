#!/usr/bin/env python3
"""Decidable prose and include rules no other scripts/gates/*.py enforces.

    python scripts/gates/check_style_audit.py [--fix] [--strict] [--rule ID] [--file TEXT]

One function per rule, registered by decorator (@doc_rule, @c_comment_rule,
@c_line_rule, @text_rule) with an id, a severity, and an optional fixer. A
finding prints as `path:line: [SEVERITY RULE-ID] message`. ERROR fails the
run; WARN prints but only fails under --strict. `--fix` applies every rule's
fixer, then re-audits, so the exit code reflects what actually remains.
`--rule` runs one rule; `--file` scopes to files whose path contains the
given text and exits 2 (never a false "clean") if that matches nothing.
`--paths FILE...` checks exact paths while retaining tree context.

Two rules a script cannot judge reliably were left out rather than scripted
badly: journey words in prose docs (find_narrative_comments.py's own
SIGNS list is, by its own docstring, "a WORKLIST, NOT A VERDICT"; tested
against this tree's docs it is nearly all ordinary technical prose, not
narration) and whether a comment should exist at all (docs/c-style/README.md
calls that judgement, not a length or a name). HEADING-COMMENT is WARN for
the same reason: a worklist, not a verdict.
"""
import functools
import pathlib
import re
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from c_includes import resolve_include as resolve_quoted_include  # noqa: E402
from c_comments import excluded, blank_comments, scan  # noqa: E402
from check_doc_citations import documentation  # noqa: E402
from check_doc_constants import ESCAPE as DOC_CONSTANTS_ESCAPE  # noqa: E402
from check_doc_index import blank_fences  # noqa: E402
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "launcher/tools/render"))
from generated_blocks import MARKER as GENERATED_BLOCK  # noqa: E402
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "launcher/tools"))
from build import package_requires, packages  # noqa: E402
from build.apps import app_part  # noqa: E402
import strip_comment_rules  # noqa: E402
from tracked import tracked_files  # noqa: E402

ERROR, WARN = "ERROR", "WARN"

BINARY_EXTENSIONS = {".png", ".jpg", ".jpeg", ".gif", ".ico", ".bmp", ".otf", ".ttf",
                     ".woff", ".woff2", ".xcf", ".bin", ".elf", ".o", ".a", ".exe",
                     ".dll", ".pyc", ".gz", ".zip", ".wav", ".mp3", ".mp4", ".pdf", ".glb"}


# Generated checks consume compressed captures; snapshot them without treating them as text.
DATA_EXTENSIONS = {".gz"}


class Finding:
    def __init__(self, path, line, rule_id, severity, message, fixable):
        self.path = path
        self.line = line
        self.rule_id = rule_id
        self.severity = severity
        self.message = message
        self.fixable = fixable

    def __str__(self):
        tag = f"[{self.severity} {self.rule_id}]" + (" (--fix)" if self.fixable else "")
        return f"{self.path}:{self.line}: {tag} {self.message}"


class Rule:
    def __init__(self, rule_id, severity, kind, func, fixer):
        self.id = rule_id
        self.severity = severity
        self.kind = kind
        self.func = func
        self.fixer = fixer


RULES = []


def _register(kind):
    def make_decorator(rule_id, severity=ERROR, fixer=None):
        def decorator(func):
            RULES.append(Rule(rule_id, severity, kind, func, fixer))
            return func
        return decorator
    return make_decorator


# Every rule function takes (root, path, data), or, for a c_comment_rule,
# (root, path, text, comments), since a comment rule sometimes needs the raw
# text too (brace context, say) and the walker already read it, and yields
# (line, message) pairs. Every fixer takes (root, path, text) and returns
# the file's new text, or None to leave it alone, the same shape
# regardless of which walker found the violation, so --fix does not need to
# know which kind of rule it is re-running.
doc_rule = _register("doc")
c_comment_rule = _register("c_comment")
c_line_rule = _register("c_line")
text_rule = _register("text")


def relpath(root, path):
    return pathlib.Path(path).resolve().relative_to(pathlib.Path(root).resolve()).as_posix()


# Walkers: each reads a file exactly once and hands every registered rule of
# its kind the same parsed data.

def _doc_walk(root, files=None):
    for path in documentation(root):
        if files is not None and relpath(root, path) not in files:
            continue
        raw = path.read_text(encoding="utf-8", errors="replace").splitlines()
        yield path, raw


def _c_walk(root, files=None):
    """First-party .c/.h files: vendored trees and generated files keep
    their upstream or generator-owned form, the same exemption
    check-format.sh and c_comments.excluded() give them."""
    root = pathlib.Path(root)
    for rel in tracked_files(root, ["*.c", "*.h"]):
        if files is not None and rel not in files:
            continue
        if any(rel.startswith(e) for e in excluded()):
            continue
        path = root / rel
        if not path.is_file():
            continue
        yield path, path.read_text(encoding="utf-8", errors="replace")


def _text_walk(root, files=None):
    root = pathlib.Path(root)
    for rel in tracked_files(root, []):
        if files is not None and rel not in files:
            continue
        p = pathlib.Path(rel)
        if p.suffix.lower() in BINARY_EXTENSIONS:
            continue
        full = root / rel
        if not full.is_file():
            continue
        try:
            text = full.read_text(encoding="utf-8", errors="strict")
        except (UnicodeDecodeError, OSError):
            continue
        yield full, text


# RULE: a tracker issue id or a git commit hash names a system this repo
# does not keep in sync with itself; it belongs in bd/beads or git, never
# in tracked text. A bd id is a short, 3-4 character code (autana-zq7,
# autana-qqx); "bd " in front is optional. The only real compound word this
# repo's own CLI names that shape, "autana-cli", is excluded by name,
# since every other one ("autana-screenshot", "autana-device",
# "autana-monitor") is already too long to match. A 40-character hex run is
# an unambiguous full SHA; a 12-character build id is perf provenance, not
# a tracker reference.

TRACKER_ID = re.compile(
    r"\bbd\s+(?:show\s+|graph\s+)?autana-[a-zA-Z0-9]+\b|\bautana-(?!cli\b)[a-z0-9]{3,4}\b")
COMMIT_SHA = re.compile(r"\b[0-9a-f]{40}\b")


def _tracker_refs(text):
    for pattern, what in ((TRACKER_ID, "a tracker issue id"), (COMMIT_SHA, "a git commit hash")):
        m = pattern.search(text)
        if m:
            yield what, m.group(0)


TRACKER_REF_ACTION = "move it to the PR body or bd and state the fact itself"


@doc_rule("TRACKER-REF")
def rule_tracker_ref_doc(root, path, raw_lines):
    for number, line in enumerate(blank_fences(raw_lines), 1):
        for what, value in _tracker_refs(line):
            yield number, f"names {what} ({value}) - {TRACKER_REF_ACTION}"


@c_comment_rule("TRACKER-REF")
def rule_tracker_ref_comment(root, path, text, comments):
    for c in comments:
        for what, value in _tracker_refs(c.text):
            yield c.line, f"comment names {what} ({value}) - {TRACKER_REF_ACTION}"


# RULE: a calendar date in a C comment or a doc records when something was
# true, not what is true now. scripts/gates/doc_review_ledger.txt is a genuine dated
# log; as a .txt it is outside this rule's .md/.c/.h scope.

DATE = re.compile(r"\b(?:19|20)\d{2}-\d{2}-\d{2}\b")


@doc_rule("CALENDAR-DATE")
def rule_calendar_date_doc(root, path, raw_lines):
    for number, line in enumerate(blank_fences(raw_lines), 1):
        m = DATE.search(line)
        if m:
            yield number, f"carries a calendar date ({m.group(0)}) - state the constraint, not when it was measured"


@c_comment_rule("CALENDAR-DATE")
def rule_calendar_date_comment(root, path, text, comments):
    for c in comments:
        m = DATE.search(c.text)
        if m:
            yield c.line, f"comment carries a calendar date ({m.group(0)}) - state the constraint, not when it was measured"


# RULE: "living document" says only that the text changes, which git log
# already says, more precisely, for free.

LIVING_DOCUMENT = re.compile(r"living document", re.I)


@doc_rule("LIVING-DOC")
def rule_living_document(root, path, raw_lines):
    for number, line in enumerate(blank_fences(raw_lines), 1):
        if LIVING_DOCUMENT.search(line):
            yield number, '"living document" - say what the doc covers, not that it changes'


# RULE: includes are layer-qualified (docs/c-style/README.md, "Headers and
# module boundaries"): "gfx/gfx.h", never "../../gfx/gfx.h" or a bare
# "gfx.h", even between two files in the same folder. launcher/main is a
# registered include root (INCLUDE_DIRS "." in CMakeLists.txt, and -I
# "$MAIN_DIR" in run_tests.sh), so every header under one of the shared
# layers, or living at main/'s own root, is reachable without a dot; so is
# each package's include/ (launcher/tools/build/packages.py), whose headers
# are spelled from that root inside the package too.
#
# resolve_include() finds the file the way the compiler does: next to the
# including file first, then from the include roots, so an app's own local
# header (found next to it) is never mistaken for a layer header that
# merely shares its basename, and a fix always rewrites to the one spelling
# that file actually has.

INCLUDE = re.compile(r'^\s*#include\s+"([^"]+)"')
ANY_INCLUDE = re.compile(r'^\s*#\s*include\s+[<"]([^>"]+)[>"]')


def include_roots(root):
    return tuple(path.resolve() for path in packages.include_roots(pathlib.Path(root) / "launcher"))


def resolve_include(root, path, inc):
    resolved = resolve_quoted_include(path.resolve().as_posix(), inc,
                                      tuple(path.as_posix() for path in include_roots(root)),
                                      lambda candidate: pathlib.Path(candidate).is_file())
    return pathlib.Path(resolved) if resolved else None


def package_of(root, path):
    """The folder of the launcher/packages/<name>/ package holding `path`, or None."""
    packages_dir = (pathlib.Path(root) / "launcher/packages").resolve()
    try:
        return packages_dir / path.resolve().relative_to(packages_dir).parts[0]
    except (ValueError, IndexError):
        return None


def _layer_root_files(root):
    return {p.name for p in (pathlib.Path(root) / "launcher/main").glob("*.h")}


def _include_layer_violations(root, path, text):
    """(line_no, include_string, canonical_spelling, message) for each
    include whose written spelling differs from the layer-qualified path
    the file it resolves to actually has."""
    main_dir = (pathlib.Path(root) / "launcher/main").resolve()
    package = package_of(root, path)
    if package is None:
        try:
            rel_to_main = path.resolve().relative_to(main_dir)
        except ValueError:
            return
        if rel_to_main.parts[:1] == ("apps",) and "tools" in rel_to_main.parts:
            return  # apps/*/tools/ is excluded from the firmware glob build entirely
    root_files = _layer_root_files(root)
    roots = include_roots(root)
    for number, line in enumerate(text.splitlines(), 1):
        m = INCLUDE.match(line)
        if not m:
            continue
        inc = m.group(1)
        resolved = resolve_include(root, path, inc)
        if resolved is None:
            continue
        include_root = next((r for r in roots if resolved.is_relative_to(r)), None)
        if include_root is None:
            continue
        target_rel = resolved.relative_to(include_root)
        top = target_rel.parts[0] if target_rel.parts else ""
        canonical = target_rel.as_posix()
        if inc != canonical and (include_root != main_dir or top in LAYER_DIRS or canonical in root_files):
            yield number, inc, canonical, f'"{inc}" is not layer-qualified - use "{canonical}"'


@c_line_rule("INCLUDE-LAYER", fixer=lambda root, path, text: _fix_include_layer(root, path, text))
def rule_include_layer(root, path, text):
    for number, _old, _new, message in _include_layer_violations(root, path, text):
        yield number, message


def _fix_include_layer(root, path, text):
    fixes = {old: new for _line, old, new, _msg in _include_layer_violations(root, path, text)}
    if not fixes:
        return None
    lines = text.splitlines(keepends=True)
    changed = False
    for i, line in enumerate(lines):
        m = INCLUDE.match(line)
        if m and m.group(1) in fixes:
            lines[i] = line.replace(f'"{m.group(1)}"', f'"{fixes[m.group(1)]}"', 1)
            changed = True
    return "".join(lines) if changed else None


@c_line_rule("PMU-OWNER")
def pmu_owner(root, path, text):
    rel = pathlib.PurePosixPath(relpath(root, path))
    if rel.parts[:2] != ("launcher", "main") or len(rel.parts) < 3 or rel.parts[2] in ("power", "board"):
        return
    for number, line in enumerate(text.splitlines(), 1):
        if re.search(r"\b(?:BOARD_PMU_I2C_ADDR|AXP2101_[A-Za-z0-9_]*)\b", line):
            yield number, "PMU registers and address belong to power/"


# RULE: a folder may include only a strictly lower tier of
# docs/firmware/Layers.md (LAYER_ROWS below, one string per
# row, top first, grouped into system, engine and platform bands; folders
# in a row share a tier). A system header such
# as "driver/temperature_sensor.h" never resolves to a layer, and a package
# (launcher/packages/) sits under every row: PACKAGE-INCLUDE keeps it there.

LAYER_BANDS = (
    ("system", ("apps", "shell", "boot selftest", "app")),
    ("engine", ("ui scene haptics", "render", "draw anim sound", "asset")),
    ("platform", ("input audio", "power", "display", "gfx", "services", "profile", "console", "core", "board",
                  "build")),
)
LAYER_ROWS = tuple(row for _band, rows in LAYER_BANDS for row in rows)
LAYER_TIER = {layer: tier for tier, row in enumerate(LAYER_ROWS) for layer in row.split()}
LAYER_DIRS = tuple(layer for layer in LAYER_TIER if layer != "apps")

INCLUDE_DIRECTION_EXCEPTIONS = {}


def layer_of(parts):
    if app_part(pathlib.PurePosixPath("launcher/main", *parts)):
        return "apps"
    return parts[0] if parts else None


def _layer_dirs_match(root):
    """A folder under launcher/main/ this table has never heard of makes
    every tier comparison below it a guess; fail loudly rather than
    silently trust an order that might already be wrong. A LAYER_TIER entry
    with no folder on disk (a fixture, or a layer deleted since) is not
    this check's business, and nor is a folder holding no C, which is
    content (pack sources) and has no tier."""
    main_dir = pathlib.Path(root) / "launcher/main"

    def has_code(folder):
        return any(path.suffix in (".c", ".h") for path in folder.rglob("*"))

    found = {d.name for d in main_dir.iterdir() if d.is_dir() and has_code(d)}
    unknown = sorted(found - set(LAYER_TIER))
    if unknown:
        raise ValueError(
            f"launcher/main has folder(s) {unknown} that LAYER_ROWS (check_style_audit.py) "
            "does not know about - add them to the tier table (and to "
            "docs/firmware/Layers.md) before this rule can trust its own order.")


def _main_include_layers(root, path, text):
    """(line, include, target layer) for each include of `text` that resolves
    to a folder of launcher/main/, quoted or angled."""
    main_dir = (pathlib.Path(root) / "launcher/main").resolve()
    for number, line in enumerate(text.splitlines(), 1):
        m = ANY_INCLUDE.match(line)
        if not m:
            continue
        resolved = resolve_include(root, path, m.group(1))
        if resolved is None or not resolved.resolve().is_relative_to(main_dir):
            continue
        yield number, m.group(1), layer_of(resolved.resolve().relative_to(main_dir).parts)


@c_line_rule("INCLUDE-DIRECTION")
def rule_include_direction(root, path, text):
    main_dir = pathlib.Path(root) / "launcher/main"
    try:
        rel_to_main = path.resolve().relative_to(main_dir.resolve())
    except ValueError:
        return
    parts = rel_to_main.parts
    source_layer = layer_of(parts)
    if source_layer == "apps" or source_layer not in LAYER_TIER:
        return
    _layer_dirs_match(root)
    source_tier = LAYER_TIER[source_layer]
    source_base = (rel_to_main.parent / rel_to_main.stem).as_posix()
    for number, inc, target_layer in _main_include_layers(root, path, text):
        if target_layer is None or target_layer == source_layer or target_layer not in LAYER_TIER:
            continue
        if LAYER_TIER[target_layer] > source_tier:
            continue
        if (source_base, target_layer) in INCLUDE_DIRECTION_EXCEPTIONS:
            continue
        yield number, (f'"{inc}" reaches from {source_layer}/ (tier {source_tier}) into {target_layer}/ '
                       f"(tier {LAYER_TIER[target_layer]}) - move the shared piece down a tier, or add an "
                       "INCLUDE_DIRECTION_EXCEPTIONS entry citing the doc section that draws it")


# RULE: an app (a folder of the top row, launcher/main/apps/) uses the engine, the platform and the app contract, and never the
# rest of the system band: docs/plans/Engine-Layer-Design-Sketch.md's tier
# rule. The app contract is the system band's lowest row, so the rule names
# no folder. A shell app (shell/apps/) shares the apps tier for
# INCLUDE-DIRECTION but ships with the shell, so it may reach the system
# band. Both include rules below read C and C++ sources alike.

INCLUDE_RULE_SUFFIXES = (".c", ".h", ".cpp")

SYSTEM_ROWS = dict(LAYER_BANDS)["system"]
APPS_ROW = SYSTEM_ROWS[0].split()
SYSTEM_BAND_REFUSED = {layer for row in SYSTEM_ROWS[1:-1] for layer in row.split()}


@text_rule("SYSTEM-BAND")
def rule_system_band(root, path, text):
    root_path = pathlib.Path(root).resolve()
    main_dir = root_path / "launcher/main"
    resolved_path = path.resolve()
    if path.suffix not in INCLUDE_RULE_SUFFIXES or not resolved_path.is_relative_to(root_path):
        return
    parts = resolved_path.relative_to(main_dir).parts if resolved_path.is_relative_to(main_dir) else ()
    if not (parts[:1] and parts[0] in APPS_ROW):
        return
    for number, inc, target_layer in _main_include_layers(root, path, text):
        if target_layer in SYSTEM_BAND_REFUSED:
            yield number, (f'"{inc}" reaches into {target_layer}/, the system band - an app uses '
                           "the engine, the platform and the app contract (app/)")


# RULE: the UI backend stays behind ui/'s face. A header is the backend's
# when it is one of the vendored backend component's own headers, or
# includes one, directly or through another header. Outside launcher/main/ui/
# no firmware source (apps, shell, their tests and tools) includes the backend or a header of ui/ or of the harness that is
# the backend's, so swapping the backend touches ui/ only. A header inside
# the rule's reach that leaks is reported at its own include, not again at
# every file including it.
# launcher/test/ and launcher/tools/ are the engine's own harness and test
# the seam itself, so they are outside the rule.

UI_BACKEND_COMPONENT = "launcher/components/microui"
UI_FACE_SCOPES = ("launcher/main",)
# Where an app's suite finds the harness's own headers (run_tests.sh's -I).
UI_HARNESS_INCLUDE = "launcher/test"


def _resolve_with_harness(root, path, inc):
    resolved = resolve_include(root, path, inc)
    harness = pathlib.Path(root) / UI_HARNESS_INCLUDE / inc
    return resolved if resolved is not None or not harness.is_file() else harness


@functools.lru_cache(maxsize=None)
def _backend_headers(root):
    return {p.name for p in (pathlib.Path(root) / UI_BACKEND_COMPONENT).rglob("*.h")}


# Whether a header reaches the backend, per root, so each header is read
# once per run however many files include it.
_BACKEND_REACH = {}


def _is_backend(root, resolved, inc, backend):
    """True if the include `inc`, found at `resolved` (None when it resolves
    nowhere, as the backend's own include root does not), is a backend header."""
    if resolved is None:
        return pathlib.PurePosixPath(inc).name in backend
    return resolved.resolve().is_relative_to((pathlib.Path(root) / UI_BACKEND_COMPONENT).resolve())


def _reaches_backend(root, header, backend, seen):
    """True if `header` (a resolved path) includes a backend header, at any depth."""
    key = header.resolve()
    if key in seen:
        return seen[key]
    seen[key] = False
    try:
        text = key.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return False
    for line in text.splitlines():
        m = ANY_INCLUDE.match(line)
        if not m:
            continue
        inc = m.group(1)
        resolved = _resolve_with_harness(root, key, inc)
        if _is_backend(root, resolved, inc, backend) or (
                resolved is not None and _reaches_backend(root, resolved, backend, seen)):
            seen[key] = True
            return True
    return False


def _in_face_scope(root_path, path):
    rel = path.resolve().relative_to(root_path).as_posix()
    return any(rel.startswith(scope + "/") for scope in UI_FACE_SCOPES) and not rel.startswith("launcher/main/ui/")


def _ui_backend_violations(root, path, text):
    root_path = pathlib.Path(root).resolve()
    if not path.resolve().is_relative_to(root_path) or not _in_face_scope(root_path, path):
        return
    backend = _backend_headers(str(root_path))
    seen = _BACKEND_REACH.setdefault(root_path, {})
    for number, line in enumerate(text.splitlines(), 1):
        m = ANY_INCLUDE.match(line)
        if not m:
            continue
        inc = m.group(1)
        resolved = _resolve_with_harness(root, path, inc)
        if _is_backend(root, resolved, inc, backend):
            yield number, f'"{inc}" is the UI backend - use ui/ui.h, the face'
        elif resolved is None or _in_face_scope(root_path, resolved):
            continue
        elif _reaches_backend(root, resolved, backend, seen):
            yield number, (f'"{inc}" exposes the UI backend ({relpath(root, resolved)} reaches '
                           f"{UI_BACKEND_COMPONENT}) - use ui/ui.h, the face")


@text_rule("UI-BACKEND")
def rule_ui_backend(root, path, text):
    if path.suffix in INCLUDE_RULE_SUFFIXES:
        yield from _ui_backend_violations(root, path, text)


# RULE: a package (launcher/packages/<name>/) sits under every layer, so it
# includes only itself and the packages it declares in its own CMakeLists.txt
# (`set(<name>_requires ...)`, launcher/tools/build/package_requires.py), theirs in
# turn: no #include, quoted or angled, may reach a tracked header of the
# firmware or of an undeclared package. Tests and benchmarks may also use
# testkit. Unresolved headers belong to the toolchain or build (<stdint.h>,
# "sdkconfig.h"); the isolated compile check rejects unresolved engine headers.
# launcher/test/check_packages_alone.sh compiles each package file with only
# those include roots, which catches what no resolver here can see.
# PACKAGE-REQUIRES keeps the declarations a one-way graph of real packages.
# PACKAGE-CONFIG keeps first-party settings with their declaring package.
# Symbols outside the own prefix and absent from first-party Kconfig belong to ESP-IDF.
# Package Kconfig declarations require the package prefix; comments are ignored.



@c_line_rule("PACKAGE-INCLUDE")
def rule_package_include(root, path, text):
    package = package_of(root, path)
    if package is None:
        return
    reachable = package_requires.reachable(package)
    if path.resolve().relative_to(package).parts[0] in ("tests", "bench"):
        reachable = [*reachable, package.parent / "testkit"]
    for number, line in enumerate(text.splitlines(), 1):
        m = ANY_INCLUDE.match(line)
        if not m:
            continue
        resolved = resolve_include(root, path, m.group(1))
        if resolved is not None and not any(resolved.is_relative_to(p) for p in reachable):
            yield number, (f'"{m.group(1)}" reaches outside the {package.name} package '
                           f"({relpath(root, resolved)}) - a package includes only itself "
                           f"and the packages its CMakeLists.txt sets in {package.name}_requires")


@text_rule("PACKAGE-REQUIRES")
def rule_package_requires(root, path, text):
    package = package_of(root, path)
    if package is None or path.name != "CMakeLists.txt" or path.parent.resolve() != package:
        return
    declared = package_requires.requires_line(package.name)
    any_declaration = re.compile(r"^\s*set\(\s*" + re.escape(package.name) + r"_requires\b")
    number = 1
    for n, line in enumerate(text.splitlines(), 1):
        if any_declaration.match(line):
            number = n
            if not declared.match(line):
                yield n, (f"{package.name}_requires is read only as one unindented line of bare package "
                          f"names: set({package.name}_requires math)")
    for name in package_requires.requires(package):
        dep = package.parent / name
        if name == package.name:
            yield number, f"{package.name} requires itself"
        elif not (dep / "include").is_dir():
            yield number, f"{package.name} requires {name}, which is no package (no launcher/packages/{name}/include)"
        elif package in package_requires.reachable(dep):
            yield number, f"{package.name} requires {name}, which requires {package.name} back - dependencies run one way"


PACKAGE_CONFIG_SYMBOL = re.compile(r"\bCONFIG_[A-Za-z0-9_]+\b")
PACKAGE_KCONFIG_ENTRY = re.compile(r"^\s*(?:menuconfig|config)\s+([A-Za-z0-9_]+)\b")
PACKAGE_BUILD_TOKEN = re.compile(r'"(?:\\.|[^"\\])*"|#?\[(=*)\[.*?\]\1\]|#[^\n]*', re.DOTALL)


@functools.lru_cache(maxsize=None)
def first_party_config_owners(root):
    owners = {}
    for rel in tracked_files(root, ["launcher/**/Kconfig*"]):
        path = pathlib.Path(root) / rel
        if not path.is_file():
            continue
        package = package_of(root, path)
        for line in path.read_text(encoding="utf-8").splitlines():
            entry = PACKAGE_KCONFIG_ENTRY.match(line)
            if entry:
                owners["CONFIG_" + entry.group(1)] = package
    return owners


@text_rule("PACKAGE-CONFIG")
def rule_package_config(root, path, text):
    package = package_of(root, path)
    if package is None:
        return
    if path.suffix in (".c", ".h"):
        code = blank_comments(text)
    elif path.name in ("CMakeLists.txt", "Kconfig"):
        code = PACKAGE_BUILD_TOKEN.sub(
            lambda m: "".join("\n" if ch == "\n" else " " for ch in m.group())
            if m.group().startswith("#") else m.group(), text)
    else:
        return
    prefix = package.name.upper() + "_"
    owners = first_party_config_owners(root)
    for number, line in enumerate(code.splitlines(), 1):
        for symbol in PACKAGE_CONFIG_SYMBOL.findall(line):
            if symbol.startswith("CONFIG_" + prefix) and owners.get(symbol) != package:
                yield number, f"{symbol} is not declared in the {package.name} package Kconfig"
            elif symbol in owners and owners[symbol] != package:
                yield number, f"{symbol} is outside the {package.name} package - use CONFIG_{prefix} settings"
        entry = PACKAGE_KCONFIG_ENTRY.match(line) if path.name == "Kconfig" else None
        if entry and not entry.group(1).startswith(prefix):
            yield number, f"{entry.group(1)} is outside the {package.name} package - name Kconfig entries {prefix}"


# RULE: a personal home-directory path, or one machine's ESP-IDF checkout,
# baked into tracked source only works on the machine that wrote it.
# launcher/tools/build/espressif.py's espressif_tools_root() and idf_python(), and
# idf.sh's idf_default_export(), are the portable forms.

# The ESP-IDF branch matches any drive path that names an esp-idf checkout,
# not one installer's folder layout. It stays off root-anchored paths: the
# portable $HOME/esp/esp-idf default and github.com/espressif/esp-idf both
# contain the name and are fine.
PERSONAL_PATH = re.compile(
    r"C:\\Users\\[A-Za-z0-9][A-Za-z0-9_.-]*|/home/[A-Za-z0-9][A-Za-z0-9_.-]*|/Users/[A-Za-z0-9][A-Za-z0-9_.-]*"
    r"|(?<![\w/])[A-Za-z]:[\\/][^\s\"'`;|()]*?esp-idf[\w.-]*")


@text_rule("PERSONAL-PATH")
def rule_personal_path(root, path, text):
    for number, line in enumerate(text.splitlines(), 1):
        m = PERSONAL_PATH.search(line)
        if m:
            yield number, f"{m.group(0)} is machine-specific - use ~, or launcher/tools/build/espressif.py or IDF_PATH, for an ESP-IDF path"


# RULE: an importer, exporter, baker or measuring tool takes a model as an
# argument and is never named after one, or every new model needs its own copy
# and every model change a clean-up. A model is any tracked .blend, .glb, .gltf,
# .obj or .fbx, wherever it lives; a tool is code under a `tools` directory or
# scripts/. An app's own runtime code may name the content it shows.

MODEL_SUFFIXES = (".blend", ".glb", ".gltf", ".obj", ".fbx")
TOOL_CODE_SUFFIXES = {".py", ".c", ".h", ".cc", ".cpp", ".sh", ".mjs", ".js", ".ps1"}


def _model_names(root):
    return {pathlib.PurePosixPath(rel).stem.lower() for rel in tracked_files(root, [])
            if rel.lower().endswith(MODEL_SUFFIXES)}


@text_rule("MODEL-NAMED-TOOL")
def rule_model_named_tool(root, path, text):
    rel = pathlib.PurePosixPath(relpath(root, path))
    if rel.suffix.lower() not in TOOL_CODE_SUFFIXES or not ("tools" in rel.parts or rel.parts[0] == "scripts"):
        return
    for model in _model_names(root):
        if model in rel.name.lower():
            yield 1, f"named after the model {model} - take the model as an argument and name the tool for what it does"


# RULE: an HTML comment in a doc renders as nothing. Only the gates' own
# escape markers, a generator's BEGIN/END GENERATED pair and
# generated_blocks.py's named block markers are allowed;
# anything else is a stray aside nobody will see. Scanned across the whole
# file (not line by line) so a comment wrapped across two lines is still
# caught, with fenced code blanked first so a shell transcript's own
# "<!--"-shaped text can't be mistaken for one.

HTML_COMMENT = re.compile(r"<!--.*?-->", re.S)
KNOWN_MARKERS = re.compile(
    re.escape(DOC_CONSTANTS_ESCAPE) + "|" +
    r"(?:BEGIN|END)\s+GENERATED|" + GENERATED_BLOCK.pattern)


# RULE: a working copy written with CRLF. .gitattributes normalises it on
# staging and CI reads the index, so this never reaches a commit, but a tool
# that rewrites a file whole leaves the checkout mixed, and the next reader
# sees a whole-file diff that is not there. A WARN with a fixer: --fix
# normalises it and nobody spends a thought on it.

LINE_ENDINGS_ACTION = "written with CRLF - run --fix, or git add --renormalize ."


@text_rule("LINE-ENDINGS", severity=WARN,
           fixer=lambda root, path, text: _fix_line_endings(path))
def rule_line_endings(root, path, text):
    raw = pathlib.Path(path).read_bytes()
    if b"\r\n" in raw:
        yield raw[:raw.index(b"\r\n")].count(b"\n") + 1, LINE_ENDINGS_ACTION


def _fix_line_endings(path):
    """Rewrites in place and returns None: the caller's text-in/text-out
    fixer contract would put the endings back on the way through."""
    path = pathlib.Path(path)
    raw = path.read_bytes()
    if b"\r\n" in raw:
        path.write_bytes(raw.replace(b"\r\n", b"\n"))
    return None


# RULE: UTF-8 read as Windows-1252 and written back turns one character such
# as a middle dot or an arrow into two or three accented ones, which breaks
# rendered text and Mermaid labels. A run is
# flagged only when its characters map back to bytes that decode as exactly
# one UTF-8 character, so ordinary accented text never matches.

CP1252_BYTE = {}
for _byte in range(0x80, 0x100):
    try:
        CP1252_BYTE[bytes([_byte]).decode("cp1252")] = _byte
    except UnicodeDecodeError:
        CP1252_BYTE[chr(_byte)] = _byte
CONTINUATION = "".join(re.escape(c) for c, b in CP1252_BYTE.items() if b < 0xC0)
MOJIBAKE = re.compile(f"[\u00c2-\u00df][{CONTINUATION}]|[\u00e0-\u00ef][{CONTINUATION}]{{2}}"
                      f"|[\u00f0-\u00f4][{CONTINUATION}]{{3}}")
MOJIBAKE_ACTION = "UTF-8 decoded as Windows-1252 - run --fix, and read and write text as UTF-8"


def _unmangle(run):
    try:
        return bytes(CP1252_BYTE[c] for c in run).decode("utf-8")
    except UnicodeDecodeError:
        return None


def _fix_mojibake(text):
    return MOJIBAKE.sub(lambda m: _unmangle(m.group(0)) or m.group(0), text)


@text_rule("MOJIBAKE", fixer=lambda root, path, text: _fix_mojibake(text))
def rule_mojibake(root, path, text):
    for number, line in enumerate(text.splitlines(), 1):
        for m in MOJIBAKE.finditer(line):
            fixed = _unmangle(m.group(0))
            if fixed is not None:
                yield number, f"{m.group(0)!r} should be {fixed!r} - {MOJIBAKE_ACTION}"
                break


# RULE: `$?` on the line after a compound statement is that compound's own
# status, not the command a reader has in mind. An `if` whose condition fails
# and which has no `else` exits 0, so `if cmd; then return 0; fi` followed by
# `rc=$?` or `exit $?` reports success for a run that failed. Capture the
# status inside the branch instead: `cmd && return 0; rc=$?`.

SHELL_FILE = re.compile(r"\.sh$|\.bash$")
SHELL_SHEBANG = re.compile(r"^#!.*\b(?:ba|da|k|z)?sh\b")
COMPOUND_END = re.compile(r"^(?:fi|done|esac|\})\s*(?:#.*)?$")
STATUS_READ = re.compile(r"^(?:exit|return)\s+\$\?\s*$|^[A-Za-z_][A-Za-z0-9_]*=\$\?\s*$")

SHELL_STATUS_ACTION = ("$? here is the compound's status, not the command's - "
                       "capture it inside the branch (`cmd && return 0; rc=$?`)")


def _is_shell(path, text):
    return bool(SHELL_FILE.search(str(path))
                or SHELL_SHEBANG.match(text.splitlines()[0] if text else ""))


@text_rule("SHELL-COMPOUND-STATUS")
def rule_shell_compound_status(root, path, text):
    if not _is_shell(path, text):
        return
    previous = ""
    for number, raw in enumerate(text.splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if STATUS_READ.match(line) and COMPOUND_END.match(previous):
            yield number, line + " after `" + previous + "`: " + SHELL_STATUS_ACTION
        previous = line


@doc_rule("STRAY-HTML-COMMENT")
def rule_stray_html_comment(root, path, raw_lines):
    blanked = "\n".join(blank_fences(raw_lines))
    for m in HTML_COMMENT.finditer(blanked):
        if KNOWN_MARKERS.search(m.group(0)):
            continue
        line = blanked.count("\n", 0, m.start()) + 1
        yield line, f"{m.group(0)[:60]} renders as nothing - delete it or say it in the text"


# RULE: a line starting "- " right after unindented, ordinary prose renders
# as a list item whether the author meant one or not. Walk back to the
# paragraph's own first line before deciding: CommonMark's lazy continuation
# lets a list item's text carry on with no indentation at all, so a
# paragraph that started with its own list marker is already a list, and a
# later "- " is a sibling item, not an accident. Skipped the same way for a
# paragraph that opens right after a closing code fence, a heading, a quote,
# or a table row, and when the line right above ends in ":" (which
# legitimately introduces a list).

BULLET = re.compile(r"^- \S")
LIST_MARKER = re.compile(r"^\s*([-*+]|\d+\.)\s")


@doc_rule("ACCIDENTAL-BULLET")
def rule_accidental_bullet(root, path, raw_lines):
    lines = blank_fences(raw_lines)
    for i, line in enumerate(lines):
        if i == 0 or not BULLET.match(line):
            continue
        prev = lines[i - 1]
        if prev.strip() == "":
            continue  # blank, or a blanked-out fenced line
        if prev != prev.lstrip():
            continue  # indented: continuing a nested block, not a bare paragraph
        if LIST_MARKER.match(prev):
            continue  # the previous line is itself a list item; a normal list
        start = i - 1
        while start > 0 and lines[start - 1].strip() != "":
            start -= 1
        para_first = lines[start]
        if LIST_MARKER.match(para_first) or para_first.lstrip().startswith(("#", ">", "|")):
            continue
        if prev.rstrip().endswith(":"):
            continue
        yield i + 1, 'starts "- " right after unindented prose - renders as a list item; rewrap the paragraph'


# RULE: two or more consecutive blank lines, or a trailing blank line at
# EOF. clang-format already owns this for C (MaxEmptyLinesToKeep, the LLVM
# default this repo never overrides, held tree-wide by format.yml on every
# push), so these only apply to docs. A generated table's own blank-line
# rhythm (docs/sand/Reaction-Table.md, between its BEGIN/END GENERATED
# markers) is the generator's to own, not this rule's.

def _generated_span(raw_lines):
    begin = end = None
    for i, line in enumerate(raw_lines):
        if "BEGIN GENERATED" in line:
            begin = i
        elif "END GENERATED" in line:
            end = i
    return (begin, end) if begin is not None and end is not None else None


@doc_rule("BLANK-LINES", fixer=lambda root, path, text: _fix_blank_lines(text))
def rule_blank_lines(root, path, raw_lines):
    span = _generated_span(raw_lines)
    run, fenced = 0, False
    for i, line in enumerate(raw_lines):
        if span and span[0] <= i <= span[1]:
            run = 0
            continue
        if line.lstrip().startswith("```"):
            fenced = not fenced
            run = 0
            continue
        if fenced:
            run = 0
            continue
        if line.strip() == "":
            run += 1
            if run == 2:
                yield i + 1, "two or more consecutive blank lines - use a single blank line"
        else:
            run = 0


def _fix_blank_lines(text):
    lines = text.splitlines(keepends=True)
    span = _generated_span([line.rstrip("\n") for line in lines])
    out, run, fenced = [], 0, False
    for i, line in enumerate(lines):
        if span and span[0] <= i <= span[1]:
            out.append(line)
            run = 0
            continue
        if line.lstrip().startswith("```"):
            fenced = not fenced
            run = 0
            out.append(line)
            continue
        if fenced:
            run = 0
            out.append(line)
            continue
        if line.strip() == "":
            run += 1
            if run > 1:
                continue
        else:
            run = 0
        out.append(line)
    new = "".join(out)
    return new if new != text else None


@doc_rule("TRAILING-BLANK-LINES", fixer=lambda root, path, text: _fix_trailing_blank(text))
def rule_trailing_blank_lines(root, path, raw_lines):
    trailing = 0
    for line in reversed(raw_lines):
        if line.strip() == "":
            trailing += 1
        else:
            break
    if trailing:
        yield len(raw_lines), f"{trailing} trailing blank line(s) at end of file - end the file on its last line of text"


def _fix_trailing_blank(text):
    lines = text.splitlines(keepends=True)
    trimmed = list(lines)
    while trimmed and trimmed[-1].strip() == "":
        trimmed.pop()
    if trimmed and not trimmed[-1].endswith("\n"):
        trimmed[-1] += "\n"
    new = "".join(trimmed)
    return new if new != text else None


# RULE: a drawn comment rule (`/*====`, `//----`) is decoration; OpenBSD
# style(9) has three comment shapes and none of them has one.
# scripts/gates/strip_comment_rules.py already proves the rewrite safe (code
# unchanged, prose unchanged word for word); this rule reuses its detection
# predicate (Comment.has_rule, from check_comment_length.py) for the check
# and its rewrite() for the fix, rather than a second implementation of
# either.

@c_comment_rule("DRAWN-COMMENT-RULE", fixer=lambda root, path, text: strip_comment_rules.rewrite(str(path), text))
def rule_drawn_comment(root, path, text, comments):
    for c in comments:
        if c.has_rule:
            yield c.line, "comment draws a rule (/*==== or //----) - style(9) has no such shape; delete the rule line (--fix does)"


# RULE: a one-line block comment of up to five plain words, sitting inside a
# function body, is a leftover section label for a block that should have
# been an extracted, named helper, docs/c-style/README.md's "needing to
# comment parts of a function separately means the function is the
# problem." The identical shape is also this tree's normal way to divide a
# FILE's top-level declarations into named groups (gfx_present.c's own
# "/* Dirty tracking */" before its dirty-tracking statics), which is a
# different, accepted use, so this only fires once brace tracking proves
# the comment sits inside a real function body, not a top-level divider or
# initializer. WARN, not ERROR: whether a comment should exist at all is
# judgement (docs/c-style/README.md's "Judgment rules"), and this is a
# worklist for a reader to weigh, not a verdict a script can hand down.

LABEL = re.compile(r"^[A-Z][a-z]+(?:\s[A-Za-z][a-z]*){0,4}$")


def _function_body_comments(text, comments):
    """The subset of `comments` sitting inside a real function body: a '{'
    at brace-depth 0 opens one only when the character before it is ')',
    the one shape a struct/array/enum initializer's '{' never has, since
    those follow '=' or a bare type keyword. Everything nested inside that
    frame (if/for/switch blocks, compound literals) inherits its state,
    counted in the one loop below."""
    blanked = blank_comments(text, mode="code")
    spans = {start: c for c in comments for start, _ in c.spans}
    inside = {}
    depth_is_fn, last_nonspace = [], ""
    for i, ch in enumerate(blanked):
        if i in spans:
            inside[id(spans[i])] = depth_is_fn[-1] if depth_is_fn else False
        if ch == "{":
            depth_is_fn.append(depth_is_fn[-1] if depth_is_fn else last_nonspace == ")")
        elif ch == "}":
            if depth_is_fn:
                depth_is_fn.pop()
        if not ch.isspace():
            last_nonspace = ch
    return {c for c in comments if inside.get(id(c))}


@c_line_rule("UNDEF-PLACEMENT")
def rule_undef_placement(root, path, text):
    if not relpath(root, path).startswith(packages.FIRST_PARTY):
        return
    comments = scan(relpath(root, path), text)
    code = blank_comments(text, mode="code")
    stack = []
    for number, line in enumerate(code.splitlines(), 1):
        directive = re.match(r"\s*#\s*(if|ifdef|ifndef|else|elif|endif|undef)\b(.*)", line)
        if not directive:
            continue
        operation, argument = directive.groups()
        if operation in ("if", "ifdef", "ifndef"):
            stack.append(operation == "ifdef" and argument.strip() == "ANALYSIS_SCAN")
        elif operation == "endif" and stack:
            stack.pop()
        elif operation in ("else", "elif") and stack:
            stack[-1] = False
        elif operation == "undef" and not any(stack):
            yield number, "#undef requires an enclosing #ifdef ANALYSIS_SCAN"


@c_comment_rule("HEADING-COMMENT", severity=WARN)
def rule_heading_comment(root, path, text, comments):
    in_function = _function_body_comments(text, comments)
    for c in comments:
        if c.is_banner or c.kind != "block" or c.lines != 1 or c not in in_function:
            continue
        t = c.text.strip()
        if LABEL.match(t):
            yield c.line, f'"{t}" is a section label inside a function body - extract a named helper instead'


def run_audit(root, rule_filter=None, file_filter=None, files=None):
    """Run every active rule's walker exactly once per matching file,
    returning (findings, files_scanned)."""
    _BACKEND_REACH.clear()
    _backend_headers.cache_clear()
    active = [r for r in RULES if not rule_filter or r.id == rule_filter]
    findings = []
    scanned = set()

    def keep(path):
        return not file_filter or file_filter in str(path).replace("\\", "/")

    def add(rel, rule, item):
        line, message = item
        findings.append(Finding(rel, line, rule.id, rule.severity, message, rule.fixer is not None))

    def audit_walk(kind, walker):
        rules = [r for r in active if r.kind == kind]
        if not rules:
            return
        for path, text in walker(root, files):
            if not keep(path):
                continue
            rel = relpath(root, path)
            scanned.add(rel)
            for r in rules:
                for item in (r.func(root, path, text) or ()):
                    add(rel, r, item)

    audit_walk("doc", _doc_walk)

    c_rules = [r for r in active if r.kind in ("c_comment", "c_line")]
    if c_rules:
        for path, text in _c_walk(root, files):
            if not keep(path):
                continue
            rel = relpath(root, path)
            scanned.add(rel)
            comments = None
            for r in c_rules:
                if r.kind == "c_comment":
                    if comments is None:
                        comments = scan(rel, text)
                    items = r.func(root, path, text, comments)
                else:
                    items = r.func(root, path, text)
                for item in (items or ()):
                    add(rel, r, item)

    audit_walk("text", _text_walk)

    findings.sort(key=lambda f: (f.path, f.line))
    return findings, len(scanned)


def run_fix(root, findings):
    """Apply every fixable finding's rule's fixer to the files it touched,
    file by file so multiple rules' fixes on one file compose."""
    by_id = {r.id: r for r in RULES}
    files = sorted({f.path for f in findings if f.fixable})
    fixed = 0
    for rel in files:
        path = pathlib.Path(root) / rel
        before = path.read_bytes()
        text = path.read_text(encoding="utf-8", errors="replace")
        original = text
        rule_ids = sorted({f.rule_id for f in findings if f.path == rel and f.fixable})
        for rule_id in rule_ids:
            fixer = by_id[rule_id].fixer
            new_text = fixer(root, path, text)
            if new_text is not None:
                text = new_text
        if text != original:
            path.write_text(text, encoding="utf-8", newline="")
        # A fixer may rewrite the file itself (line endings do not survive the
        # text contract above), so the count is what the bytes say.
        if path.read_bytes() != before:
            fixed += 1
            print(f"  fixed [{', '.join(rule_ids)}] {rel}")
    print(f"\nfixed {fixed} file(s)")


def main(argv):
    files = None
    if "--paths" in argv:
        at = argv.index("--paths")
        files, argv = argv[at + 1:], argv[:at]
    root = "."
    fix = strict = False
    rule_filter = file_filter = None
    it = iter(argv)
    for arg in it:
        if arg == "--fix":
            fix = True
        elif arg == "--strict":
            strict = True
        elif arg == "--rule":
            rule_filter = next(it, None)
        elif arg == "--file":
            file_filter = next(it, None)
        elif arg == "--root":
            root = next(it, None)
        else:
            print("usage: check_style_audit.py [--fix] [--strict] [--rule ID] [--file TEXT] [--root ROOT]",
                  file=sys.stderr)
            return 2

    if rule_filter and rule_filter not in {r.id for r in RULES}:
        print(f"no such rule: {rule_filter}", file=sys.stderr)
        return 2

    try:
        findings, scanned = run_audit(root, rule_filter, file_filter, files)
    except ValueError as error:
        print(error, file=sys.stderr)
        return 2
    if file_filter and scanned == 0:
        print(f"'{file_filter}' matched no file - nothing was audited.", file=sys.stderr)
        return 2

    if fix and any(f.fixable for f in findings):
        run_fix(root, findings)
        findings, scanned = run_audit(root, rule_filter, file_filter, files)

    for f in findings:
        print(f)
    errors = [f for f in findings if f.severity == ERROR]
    warnings = [f for f in findings if f.severity == WARN]
    print(f"{scanned} file(s) scanned, {len(errors)} error(s), {len(warnings)} warning(s)")
    return 1 if (errors or (strict and warnings)) else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

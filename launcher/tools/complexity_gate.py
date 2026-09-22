#!/usr/bin/env python3
"""
complexity_gate.py - a ratchet on clang-tidy's own
readability-function-cognitive-complexity check, not a fixed threshold.

Measures every first-party .c file under launcher/main/ (the ESP-IDF
diagnostics build's own compile database, esp-clang targeting Xtensa) and
every portable file the host test runner compiles, merged into one set
with no double counting. A .c file under launcher/main/ that neither
source measures, and that is not on EXCLUDED_MAIN_FILES below with a
reason, fails the gate by name - coverage cannot silently shrink.

The gate FAILS when a function scores above FAIL_THRESHOLD and either
rose above its last committed baseline or has no baseline entry, or when
clang-tidy reports a parse error anywhere - a partial parse can hide
functions, so it is never accepted quietly. A rise that stays at or under
FAIL_THRESHOLD only WARNS (a GitHub annotation in CI). A falling score
PASSES with a note to lower the baseline - lowering it is always an
explicit --update-baseline, never automatic. See
docs/tools/Complexity-Gate.md for the coverage accounting and the
threshold's reasoning.

USAGE
    complexity_gate.py                    the ratchet (default: --check)
    complexity_gate.py --update-baseline  overwrite the baseline with today's
                                           scores - a deliberate, human act
    complexity_gate.py --changed <ref>    scan only files git diff finds
                                           against <ref> (fast, local use)

Needs launcher/build.diag/compile_commands.json - run
launcher/tools/build_diag_check.sh first.
"""
import argparse
import glob
import json
import os
import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from espressif import espressif_tools_root  # noqa: E402  (path must be set up first)

TOOLS_DIR = Path(__file__).resolve().parent
LAUNCHER_DIR = TOOLS_DIR.parent
REPO_ROOT = LAUNCHER_DIR.parent
RUN_TESTS = LAUNCHER_DIR / "test" / "run_tests.sh"
CLANG_TIDY_CONFIG = LAUNCHER_DIR / ".clang-tidy"
BASELINE_PATH = TOOLS_DIR / "complexity_baseline.txt"
BUILD_DIR = LAUNCHER_DIR / "build.tidy"
IDF_DB_PATH = LAUNCHER_DIR / "build.diag" / "compile_commands.json"

PINNED_MAJOR = "19"

# The project's own documented standard (docs/sand/Sand-Simulation.md,
# "Broken down further") is Sonar's *default* line of 15, not the 25 the
# standalone check used - every function in main/ was driven under 15 by
# hand once already. Above it a function may not grow and a new one may not
# land; at or under it a rise is the reviewer's call, not the gate's.
FAIL_THRESHOLD = 15

# Below this fraction of the baseline's function count, something broke the
# scan itself (a flag rejected, a path silently unmatched) rather than the
# tree actually losing that many functions - the same "quiet zero" failure
# mode being replaced, one step less total. Fail loudly instead of trusting
# a number that different from history.
MIN_COVERAGE_RATIO = 0.5

# A .c file under launcher/main/ that no source below can give real flags
# to. Reviewed by hand, not grown casually - each entry needs a reason a
# reader can check.
EXCLUDED_MAIN_FILES = {
    "main/apps/sand/tools/crossflow_bench.c":
        "uses C11 timespec_get()/TIME_UTC; esp-clang's driver does not "
        "expose them under this project's -std=c11 with the MinGW headers "
        "the host route resolves against, unrelated to the Xtensa target "
        "- host gcc compiles it fine (report_crossflow.sh).",
}

# A file only a build VARIANT compiles is in no diagnostics compile database,
# so it is measured with the command of a sibling in the same folder - the
# flags it would have had - rather than excused from the gate. The variant's
# own symbol is defined for it, since what such a file calls is often
# declared only under that symbol.
VARIANT_ONLY_FILES = {
    "main/gfx/gfx_null_panel.c": ("main/gfx/gfx.c", "CONFIG_LAUNCHER_QEMU"),
    "main/console/console_inject.c": ("main/console/console.c", "CONFIG_LAUNCHER_QEMU"),
}

VENDORED_DIR_NAMES = {"components", "managed_components"}

# Vendored source -> its pristine upstream copy (a pinned submodule). A
# vendored function is measured only where this project changed its BODY
# relative to that copy; a function identical to upstream is the upstream
# author's code and outside this ratchet.
VENDORED_REFERENCES = {
    "launcher/components/microui/src/microui.c":
        "third_party/upstream/microui/src/microui.c",
    "launcher/components/microui/include/microui.h":
        "third_party/upstream/microui/src/microui.h",
    "launcher/components/small3dlib/include/small3dlib.h":
        "third_party/upstream/small3dlib/small3dlib.h",
}

# GCC-only Xtensa flags esp-clang's driver does not recognise at all - an
# unrecognised -f/-m flag is a hard parse error for clang, not a warning,
# and there is no clang equivalent needed for a syntax-only complexity
# pass. Stripped rather than worked around.
BAD_GCC_FLAGS = {
    "-fno-tree-switch-conversion",
    "-fstrict-volatile-bitfields",
    "-mdisable-hardware-atomics",
}

RESPONSE_FILE_RE = re.compile(r'@"([^"]+)"')

DIAG_RE = re.compile(
    r"^(?P<file>.+):(?P<line>\d+):\d+: warning: function '(?P<name>[^']+)' "
    r"has cognitive complexity of (?P<score>\d+) \(threshold"
)
FILE_ERROR_RE = re.compile(
    r"^(?P<file>[A-Za-z]:[\\/].+|/.+):(?P<line>\d+):(?P<col>\d+): error: (?P<msg>.+)$"
)
BARE_ERROR_RE = re.compile(r"^error: (?P<msg>.+)$")


def to_native_path(p):
    """Git Bash's `pwd` (what run_tests.sh's paths are built from) prints
    MSYS-style /c/Users/... paths; native clang-tidy.exe wants a drive
    letter. Rewrites just that leading segment - a no-op on any path that
    doesn't start with it, which is every path on a non-Windows host."""
    m = re.match(r"^/([A-Za-z])/(.*)$", p)
    if m and os.name == "nt":
        return f"{m.group(1)}:/{m.group(2)}"
    return p


def clang_tidy_major(binary):
    try:
        out = subprocess.run([binary, "--version"], capture_output=True,
                              text=True, timeout=15)
    except OSError:
        return None
    m = re.search(r"version\s+(\d+)", out.stdout)
    return m.group(1) if m else None


def candidate_clang_tidy_binaries():
    candidates = []
    env = os.environ.get("CLANG_TIDY")
    if env:
        candidates.append(env)
    for name in (f"clang-tidy-{PINNED_MAJOR}", "clang-tidy"):
        from shutil import which
        found = which(name)
        if found:
            candidates.append(found)
    exe = "clang-tidy.exe" if os.name == "nt" else "clang-tidy"
    pattern = str(espressif_tools_root() / "tools" / "esp-clang" / "*" /
                  "esp-clang" / "bin" / exe)
    candidates.extend(sorted(glob.glob(pattern), reverse=True))
    return candidates


def resolve_clang_tidy():
    allow_any = os.environ.get("CLANG_TIDY_ANY_VERSION") == "1"
    fallback = None
    for candidate in candidate_clang_tidy_binaries():
        major = clang_tidy_major(candidate)
        if major is None:
            continue
        if major == PINNED_MAJOR:
            return candidate, major
        if fallback is None:
            fallback = (candidate, major)

    if fallback is None:
        sys.exit(
            "No clang-tidy found: not on PATH, not in $CLANG_TIDY, and not "
            f"under {espressif_tools_root() / 'tools' / 'esp-clang'}. This gate needs clang-tidy "
            f"{PINNED_MAJOR}.x - ESP-IDF's bundled esp-clang carries it "
            "(install ESP-IDF, or source its export script so PATH finds "
            "it), or install LLVM 19 directly "
            f"(apt install clang-tidy-{PINNED_MAJOR} / brew install llvm@{PINNED_MAJOR})."
        )
    candidate, major = fallback
    if not allow_any:
        sys.exit(
            f"Found clang-tidy {major} ({candidate}), but this gate pins "
            f"{PINNED_MAJOR}.x, the same major scripts/gates/check-format.sh pins "
            "clang-format to and for the same reason: different majors score "
            "this check differently. Set CLANG_TIDY_ANY_VERSION=1 to run "
            "anyway (informational only - CI always uses the pinned major)."
        )
    print(f"WARNING: using clang-tidy {major}, not the pinned {PINNED_MAJOR}.x.",
          file=sys.stderr)
    return candidate, major


def find_xtensa_toolchain_root():
    """The xtensa-esp-elf GCC install esp-clang needs pointed at
    (--sysroot/--gcc-toolchain) to resolve newlib's platform_include shims
    - esp-clang carries no libc of its own for this target. Same
    glob-and-take-newest convention as the esp-clang lookup above, under
    the same ESP-IDF tools root."""
    env = os.environ.get("XTENSA_GCC_ROOT")
    if env:
        return env
    pattern = str(espressif_tools_root() / "tools" / "xtensa-esp-elf" /
                  "*" / "xtensa-esp-elf")
    matches = sorted(glob.glob(pattern), reverse=True)
    return matches[0] if matches else None


def find_host_toolchain():
    """esp-clang's own default target is a bare riscv32-esp-unknown-elf
    with no real libc at all - a host file needing <stdio.h> fails
    exactly like an unfixed Xtensa file did, unless it too is pointed at
    a real toolchain. Asks tools/find_cc.sh for the same compiler
    run_tests.sh proved these sources compile with (never a second,
    independently-guessed one), then derives that compiler's own native
    target and install root from itself - portable to whatever compiler
    a given machine's find_cc() resolves to, MinGW or a Linux system gcc
    alike."""
    find_cc_sh = TOOLS_DIR / "find_cc.sh"
    result = subprocess.run(
        ["sh", "-c", f'. "{find_cc_sh.as_posix()}"; find_cc'],
        capture_output=True, text=True)
    if result.returncode != 0 or not result.stdout.strip():
        return None, None, None
    cc = result.stdout.strip()
    from shutil import which
    resolved_cc = which(cc) or cc
    try:
        target = subprocess.run([resolved_cc, "-dumpmachine"],
                                 capture_output=True, text=True,
                                 timeout=15).stdout.strip()
    except OSError:
        return None, None, None
    if not target:
        return None, None, None
    toolchain_root = Path(resolved_cc).resolve().parent.parent
    sysroot = toolchain_root / target
    return target, str(toolchain_root), str(sysroot) if sysroot.is_dir() else None


def run_tests_print(flag):
    result = subprocess.run(["sh", str(RUN_TESTS), flag],
                             capture_output=True, text=True)
    if result.returncode != 0:
        sys.exit(
            f"run_tests.sh {flag} failed (exit {result.returncode}):\n"
            f"{result.stderr}"
        )
    return [to_native_path(line) for line in result.stdout.splitlines()
            if line.strip()]


def is_vendored(path_str):
    """launcher/components/ (microui, small3dlib), managed_components/
    (lvgl, pulled in only as a BSP dependency, never called - see
    CLAUDE.md), and the vendored Unity framework under test/framework/ -
    third-party code, out of scope for a ratchet on THIS project's own
    functions."""
    parts = Path(path_str).parts
    if any(p in VENDORED_DIR_NAMES for p in parts):
        return True
    if "framework" in parts and Path(path_str).name == "unity.c":
        return True
    return False


def _blank(text, start, end, keep_newlines=True):
    return "".join(c if (keep_newlines and c == "\n") else " "
                   for c in text[start:end])


def mask_source(text):
    """Two same-length views of a C source. `code` has comments and string
    and character literals blanked; `parse` additionally blanks preprocessor
    lines, so a brace inside a directive cannot unbalance function bodies.
    Offsets agree between the two, so a body located in `parse` is read
    back from `code` with its directives intact."""
    out = list(text)
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if c == "/" and nxt == "*":
            end = text.find("*/", i + 2)
            end = n if end < 0 else end + 2
            out[i:end] = _blank(text, i, end)
            i = end
        elif c == "/" and nxt == "/":
            end = text.find("\n", i)
            end = n if end < 0 else end
            out[i:end] = _blank(text, i, end)
            i = end
        elif c in "\"'":
            j = i + 1
            while j < n and text[j] != c and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            end = min(j + 1, n)
            out[i + 1:end - 1] = _blank(text, i + 1, end - 1)
            i = end
        else:
            i += 1
    code = "".join(out)
    parse = []
    continued = False
    for line in code.splitlines(keepends=True):
        directive = continued or line.lstrip().startswith("#")
        parse.append(_blank(line, 0, len(line)) if directive else line)
        continued = directive and line.rstrip("\r\n").endswith("\\")
    return code, "".join(parse)


IDENT_RE = re.compile(r"[A-Za-z_]\w*$")


def function_bodies(text):
    """Every file-scope function definition in a C source, as
    name -> body with all whitespace removed. A definition is a file-scope
    `{` whose previous significant character closes a parameter list; the
    name is the identifier before that list's `(`. Signature and linkage are
    deliberately excluded, so changing a function to `static inline`
    without touching its body does not count as modifying it."""
    code, parse = mask_source(text)
    bodies = {}
    depth = 0
    for i, ch in enumerate(parse):
        if ch == "{":
            if depth == 0:
                j = i - 1
                while j >= 0 and parse[j].isspace():
                    j -= 1
                if j >= 0 and parse[j] == ")":
                    k, paren = j, 0
                    while k >= 0:
                        if parse[k] == ")":
                            paren += 1
                        elif parse[k] == "(":
                            paren -= 1
                            if paren == 0:
                                break
                        k -= 1
                    m = IDENT_RE.search(parse[:k].rstrip())
                    if m:
                        close, d = i, 0
                        while close < len(parse):
                            if parse[close] == "{":
                                d += 1
                            elif parse[close] == "}":
                                d -= 1
                                if d == 0:
                                    break
                            close += 1
                        bodies[m.group(0)] = "".join(code[i:close + 1].split())
            depth += 1
        elif ch == "}":
            depth = max(depth - 1, 0)
    return bodies


def modified_vendored_functions():
    """Vendored file -> (names whose body differs from the pinned upstream
    copy or that upstream lacks, every function name found in our copy).
    Exits with the init command when a reference submodule is not checked
    out - a missing reference must never read as "nothing modified"."""
    result = {}
    for ours_rel, upstream_rel in VENDORED_REFERENCES.items():
        upstream = REPO_ROOT / upstream_rel
        if not upstream.is_file():
            sys.exit(
                f"Upstream reference {upstream_rel} is not checked out, so "
                "vendored modifications cannot be measured. Run: "
                "git submodule update --init"
            )
        ours = function_bodies((REPO_ROOT / ours_rel).read_text(
            encoding="utf-8", errors="replace"))
        theirs = function_bodies(upstream.read_text(encoding="utf-8",
                                                    errors="replace"))
        modified = {name for name, body in ours.items()
                    if theirs.get(name) != body}
        result[ours_rel] = (modified, set(ours))
    return result


INCLUDE_RE = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', re.M)


def translation_units_reaching(header, sources):
    """Measured sources that include `header` directly or through a
    first-party header that does - a header is scored by clang-tidy only
    from inside a translation unit that pulls it in."""
    candidates = [p for p in (LAUNCHER_DIR / "main").rglob("*")
                  if p.suffix in (".c", ".h")]
    candidates += [p for p in (LAUNCHER_DIR / "test").rglob("*")
                   if p.suffix in (".c", ".h")]
    includes = {}
    for path in candidates:
        text = path.read_text(encoding="utf-8", errors="replace")
        includes[path.resolve()] = {Path(inc).name for inc in INCLUDE_RE.findall(text)}
    reaching = {header.name}
    grew = True
    while grew:
        grew = False
        for path, names in includes.items():
            if path.suffix == ".h" and path.name not in reaching and names & reaching:
                reaching.add(path.name)
                grew = True
    wanted = {str(Path(s).resolve()) for s in sources}
    return sorted(str(p) for p, names in includes.items()
                  if p.suffix == ".c" and names & reaching and str(p) in wanted)


def scan_vendored(clang_tidy, db_path, sources):
    """Scores for the vendored functions this project modified, keyed like
    the first-party scan by (vendored path, name). A header is scored from
    every translation unit that reaches it and the highest score kept,
    since each includer's own #defines can change the code compiled. Every
    function clang-tidy reports in a vendored file must also be found by
    function_bodies(); a mismatch means the body comparison cannot see what
    the scan sees, and fails rather than trusting a partial answer."""
    scores = {}
    for ours_rel, (modified, found) in modified_vendored_functions().items():
        path = (REPO_ROOT / ours_rel).resolve()
        if path.suffix == ".c":
            units, header_filter = [str(path)], None
        else:
            units = translation_units_reaching(path, sources)
            header_filter = re.escape(path.name)
        if not units:
            if modified:
                sys.exit(f"FAIL: {ours_rel} has modified functions "
                         f"{sorted(modified)} but no measured file includes it.")
            continue
        cmd = [clang_tidy, "-p", str(db_path.parent), "--config-file",
               str(CLANG_TIDY_CONFIG), "--quiet"]
        if header_filter:
            cmd.append(f"--header-filter={header_filter}")
        proc = subprocess.run(cmd + units, capture_output=True, text=True)
        errors = find_parse_errors(proc.stdout)
        if errors:
            print(f"FAIL: parse error while scoring {ours_rel}:")
            for file, msg in errors:
                print(f"  {file or '(no file attributed)'}: {msg}")
            sys.exit(1)
        reported = {}
        for line in proc.stdout.splitlines():
            m = DIAG_RE.match(line)
            if not m or Path(m.group("file")).resolve() != path:
                continue
            name, score, ln = m.group("name"), int(m.group("score")), int(m.group("line"))
            if name not in reported or score > reported[name][0]:
                reported[name] = (score, ln)
        unseen = sorted(set(reported) - found)
        if unseen:
            sys.exit(f"FAIL: clang-tidy scored {unseen} in {ours_rel}, but the "
                     "body comparison against upstream did not find them - it "
                     "cannot judge what it cannot see.")
        for name in modified:
            if name in reported:
                scores[(ours_rel, name)] = reported[name]
        # clang-tidy reports nothing for a function with no control flow, or
        # one no measured translation unit compiles (a disabled #if). Neither
        # can be ratcheted, but a modified function must never vanish from
        # the report because the scan could not score it.
        for name in sorted(modified - set(reported)):
            print(f"  {ours_rel}  {name}()  modified, unscored: no control "
                  "flow, or not compiled in any measured configuration")
    return scores


def inline_response_file(cmd):
    """ESP-IDF's generated compile commands hand most flags to the
    compiler via a GCC-style @"file" response file rather than inline -
    inlined here so BAD_GCC_FLAGS can be stripped from all of it, not just
    whatever half of the command happens to sit outside the file."""
    m = RESPONSE_FILE_RE.search(cmd)
    if not m:
        return cmd
    try:
        rf_text = Path(m.group(1)).read_text(encoding="utf-8", errors="replace")
    except OSError:
        return cmd
    return cmd[:m.start()] + rf_text + cmd[m.end():]


def build_idf_entries(toolchain_root, vendored=False):
    """launcher/build.diag/compile_commands.json, restricted to this
    project's own main/ and test/ trees (excluding vendored code and
    apps/*/tools/, which the firmware never links) - or, with `vendored`,
    to only the vendored .c files in VENDORED_REFERENCES - with the flags esp-idf
    generated for xtensa-esp32s3-elf-gcc adjusted for esp-clang: the three
    GCC-only flags it does not recognise stripped, and --sysroot/
    --gcc-toolchain added so its `#include_next` chain into newlib
    resolves. Real device flags otherwise - this is what makes
    hardware-facing files (app_*.c, gfx.c, ui.c, boot/, board/, the
    input/ drivers) measurable at all."""
    if not IDF_DB_PATH.exists():
        return {}
    db = json.loads(IDF_DB_PATH.read_text(encoding="utf-8"))
    sysroot = f"{toolchain_root}/xtensa-esp-elf"
    entries = {}
    for e in db:
        path = Path(e["file"])
        try:
            rel = path.resolve().relative_to(LAUNCHER_DIR)
        except ValueError:
            continue
        parts = rel.parts
        if vendored:
            if ("launcher/" + rel.as_posix()) not in VENDORED_REFERENCES:
                continue
        elif not parts or parts[0] not in ("main", "test"):
            continue
        elif is_vendored(str(rel)) or "tools" in parts:
            continue
        cmd = inline_response_file(e.get("command", ""))
        cmd = " ".join(t for t in cmd.split() if t not in BAD_GCC_FLAGS)
        cmd += (f' --sysroot="{sysroot}" --gcc-toolchain="{toolchain_root}"'
                ' -Wno-unknown-warning-option -Wno-string-plus-int')
        entries[str(path.resolve())] = cmd
    if not vendored:
        for rel, (sibling_rel, variant_symbol) in VARIANT_ONLY_FILES.items():
            sibling = str((LAUNCHER_DIR / sibling_rel).resolve())
            target = str((LAUNCHER_DIR / rel).resolve())
            if sibling not in entries or target in entries:
                continue
            # Same folder, so only the file name differs - after either
            # separator, since a Windows database writes backslashes.
            borrowed, swaps = re.subn(
                r"(?<=[\\/])" + re.escape(Path(sibling_rel).name) + r"(?!\w)",
                lambda _m: Path(rel).name, entries[sibling])
            if swaps == 0:
                sys.exit(f"{sibling_rel}'s compile command does not name it; "
                         f"cannot borrow it for {rel}")
            entries[target] = f"{borrowed} -D{variant_symbol}=1"
    return entries


# apps/*/tools/*.c - excluded from the firmware and the host build alike
# by long-standing convention (main/CMakeLists.txt, run_tests.sh), but
# check_main_coverage()'s own rglob("*.c") below reaches them anyway, so
# parity means giving them real flags where that is possible at all: each
# already has its own working
# host compile line in a report_*.sh beside it (find_cc()'s compiler, this
# project's own headers) - -I main/apps/<app> is the one addition beyond
# the host runner's own flags every one of them needs, for its sibling
# headers (material.h, sand.h, ...).
TOOLS_FILES_EXTRA_INCLUDE = "apps/sand"


def build_tools_entries(host_flags, already_covered):
    entries = {}
    for path in sorted((LAUNCHER_DIR / "main").glob("apps/*/tools/*.c")):
        resolved = str(path.resolve())
        if resolved in already_covered:
            continue
        rel = path.resolve().relative_to(LAUNCHER_DIR).as_posix()
        if rel in EXCLUDED_MAIN_FILES:
            continue
        extra_include = (LAUNCHER_DIR / "main" / TOOLS_FILES_EXTRA_INCLUDE).as_posix()
        entries[resolved] = (["clang"] + host_flags +
                              ["-I", extra_include, Path(resolved).as_posix()])
    return entries


def build_compile_db():
    toolchain_root = find_xtensa_toolchain_root()
    if toolchain_root is None:
        sys.exit(
            "No xtensa-esp-elf GCC toolchain found under "
            "~/.espressif/tools/xtensa-esp-elf/ (or $XTENSA_GCC_ROOT) - "
            "needed to point esp-clang at the right libc headers. Install "
            "ESP-IDF for the esp32s3 target."
        )
    if not IDF_DB_PATH.exists():
        sys.exit(
            f"{IDF_DB_PATH} not found. This gate measures hardware-facing "
            "files (app_*.c, gfx.c, ui.c, boot/, board/, input/) through "
            "the diagnostics build's own compile database - run "
            "./launcher/tools/build_diag_check.sh first (a full build, "
            "several minutes, foreground), then re-run this gate."
        )

    idf_entries = build_idf_entries(toolchain_root)

    host_target, host_root, host_sysroot = find_host_toolchain()
    if host_target is None:
        sys.exit(
            "Could not resolve a host compiler via tools/find_cc.sh - "
            "needed to point esp-clang at a real host libc the same way "
            "the Xtensa toolchain above was found. Install a host "
            "compiler (see run_tests.sh's own message for how)."
        )

    host_sources = [s for s in run_tests_print("--print-sources")
                     if not is_vendored(s)]
    host_flags = run_tests_print("--print-flags") + [
        f"--target={host_target}", f"--gcc-toolchain={host_root}",
        "-Wno-unknown-warning-option",
        # A string literal offset by an int ("ABCD" + n) is legitimate,
        # deliberate pointer arithmetic in several suites (see
        # suite_ui_transform.c) - not the indexing-vs-offset typo this
        # check exists to catch.
        "-Wno-string-plus-int",
    ]
    if host_sysroot:
        host_flags.append(f"--sysroot={host_sysroot}")
    host_entries = {}
    for src in host_sources:
        resolved = str(Path(src).resolve())
        if resolved in idf_entries:
            continue
        host_entries[resolved] = ["clang"] + host_flags + [Path(resolved).as_posix()]

    tools_entries = build_tools_entries(
        host_flags, set(idf_entries) | set(host_entries))

    all_entries = {**idf_entries, **host_entries, **tools_entries}
    # Written to the database so clang-tidy can parse them, but never part
    # of the first-party source list: scan_vendored() scores only the
    # functions this project modified.
    vendored_entries = build_idf_entries(toolchain_root, vendored=True)

    BUILD_DIR.mkdir(parents=True, exist_ok=True)
    db_json = []
    for path, cmd in {**all_entries, **vendored_entries}.items():
        entry = {"directory": str(LAUNCHER_DIR), "file": path}
        # IDF entries stay a "command" string (built by text surgery on
        # ESP-IDF's own string); host/tools entries are a real argv list
        # (built here token by token) - "arguments" needs no tokenising on
        # the way back in, which a hand-built string does and a Windows
        # path's own backslashes can trip.
        entry["arguments" if isinstance(cmd, list) else "command"] = cmd
        db_json.append(entry)
    db_path = BUILD_DIR / "compile_commands.json"
    db_path.write_text(json.dumps(db_json, indent=2), encoding="utf-8")

    counts = {"idf": len(idf_entries), "host": len(host_entries),
              "tools": len(tools_entries)}
    return db_path, sorted(all_entries), counts


def check_main_coverage(measured_files):
    """Every .c file under launcher/main/, found by walking the
    filesystem - independent of any build or database. A gap that is not
    EXCLUDED_MAIN_FILES, with a reason, is a coverage regression and fails
    the gate by name rather than shrinking quietly."""
    measured = {str(Path(f).resolve()) for f in measured_files}
    on_disk = sorted((LAUNCHER_DIR / "main").rglob("*.c"))
    missing = []
    for path in on_disk:
        resolved = str(path.resolve())
        rel = path.resolve().relative_to(LAUNCHER_DIR).as_posix()
        if resolved in measured or rel in EXCLUDED_MAIN_FILES:
            continue
        missing.append(rel)
    return len(on_disk), missing


def find_parse_errors(stdout):
    errors = []
    for line in stdout.splitlines():
        m = FILE_ERROR_RE.match(line)
        if m:
            errors.append((m.group("file"), m.group("msg")))
            continue
        m = BARE_ERROR_RE.match(line)
        if m:
            errors.append((None, m.group("msg")))
    return errors


def scan(clang_tidy, db_path, files):
    if not files:
        return {}, None
    cmd = [clang_tidy, "-p", str(db_path.parent), "--config-file",
           str(CLANG_TIDY_CONFIG), "--quiet", *files]
    result = subprocess.run(cmd, capture_output=True, text=True)
    scores = {}
    for line in result.stdout.splitlines():
        m = DIAG_RE.match(line)
        if not m:
            continue
        path = Path(m.group("file"))
        try:
            rel = str(path.resolve().relative_to(REPO_ROOT)).replace("\\", "/")
        except ValueError:
            rel = str(path).replace("\\", "/")
        scores[(rel, m.group("name"))] = (int(m.group("score")),
                                           int(m.group("line")))
    return scores, result


def load_baseline(path):
    baseline = {}
    if not path.exists():
        return baseline
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line.strip() or line.startswith("#"):
            continue
        score_s, loc, name = line.split("\t")
        rel, line_s = loc.rsplit(":", 1)
        baseline[(rel, name)] = (int(score_s), int(line_s))
    return baseline


def write_baseline(path, scores):
    lines = [
        "# generated by launcher/tools/complexity_gate.py --update-baseline",
        "# score\tfile:line\tfunction",
    ]
    for (rel, name), (score, line) in sorted(
            scores.items(), key=lambda kv: (-kv[1][0], kv[0][0], kv[0][1])):
        lines.append(f"{score}\t{rel}:{line}\t{name}")
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def changed_rel_paths(ref):
    result = subprocess.run(["git", "-C", str(REPO_ROOT), "diff",
                              "--name-only", ref],
                             capture_output=True, text=True, check=True)
    return {line.strip() for line in result.stdout.splitlines() if line.strip()}


def changed_files(ref, all_sources):
    result = subprocess.run(["git", "-C", str(REPO_ROOT), "diff",
                              "--name-only", ref],
                             capture_output=True, text=True, check=True)
    changed_abs = {str((REPO_ROOT / line).resolve()) for line in
                   result.stdout.splitlines() if line.strip()}
    return [s for s in all_sources if str(Path(s).resolve()) in changed_abs]


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--update-baseline", action="store_true",
                     help="overwrite the baseline with today's scan (full scan only)")
    ap.add_argument("--changed", metavar="REF",
                     help="scan only files that differ from REF")
    ap.add_argument("--baseline", type=Path, default=BASELINE_PATH)
    args = ap.parse_args()

    if args.update_baseline and args.changed:
        sys.exit("--update-baseline needs a full scan; drop --changed.")

    clang_tidy, major = resolve_clang_tidy()
    print(f"clang-tidy {major}: {clang_tidy}")

    db_path, all_sources, counts = build_compile_db()
    print(f"compile database: {db_path} ({len(all_sources)} files - "
          f"{counts['idf']} from the diagnostics build, {counts['host']} "
          f"host-only, {counts['tools']} apps/*/tools/*.c)")

    total_on_disk, missing = check_main_coverage(all_sources)
    print(f"launcher/main/ coverage: {total_on_disk} .c files on disk, "
          f"{len(missing)} unmeasured and unexcluded")
    if missing:
        sys.exit(
            "FAIL: the following launcher/main/ files are measured by "
            "nothing and are not on EXCLUDED_MAIN_FILES:\n" +
            "\n".join(f"  {m}" for m in missing)
        )

    files = all_sources
    if args.changed:
        files = changed_files(args.changed, all_sources)
        print(f"--changed {args.changed}: {len(files)} of those files touched")

    current, proc = scan(clang_tidy, db_path, files)

    if proc is not None:
        parse_errors = find_parse_errors(proc.stdout)
        if parse_errors:
            print("FAIL: clang-tidy reported a parse error - a partial "
                  "parse can hide functions, never accepted quietly:")
            for file, msg in parse_errors:
                print(f"  {file or '(no file attributed)'}: {msg}")
            return 1

    if files and not current:
        sys.exit(
            "clang-tidy scanned {} file(s) and found ZERO functions - an "
            "empty scan is a broken scan, never a clean result. clang-tidy "
            "stderr:\n{}".format(len(files), proc.stderr[-4000:])
        )

    vendored_changed = not args.changed or any(
        rel in changed_rel_paths(args.changed) for rel in VENDORED_REFERENCES)
    if vendored_changed:
        vendored = scan_vendored(clang_tidy, db_path, all_sources)
        print(f"vendored functions modified from upstream and scored: {len(vendored)}")
        for (rel, name), (score, _) in sorted(vendored.items()):
            print(f"  {rel}  {name}()  {score}")
        current.update(vendored)

    baseline = load_baseline(args.baseline)

    if not args.changed and baseline:
        ratio = len(current) / len(baseline)
        if ratio < MIN_COVERAGE_RATIO:
            sys.exit(
                f"clang-tidy measured only {len(current)} functions; the "
                f"baseline lists {len(baseline)} ({ratio:.0%}). That is too "
                "large a drop to be real drift - the scan likely broke "
                "(a rejected flag, an unmatched path). Refusing to compare."
            )

    if args.update_baseline:
        write_baseline(args.baseline, current)
        print(f"wrote {len(current)} functions to {args.baseline}")
        return 0

    failures = []
    warnings = []
    lowered = []
    for key, (score, line) in sorted(current.items(),
                                      key=lambda kv: (-kv[1][0], kv[0])):
        rel, name = key
        if key in baseline:
            base_score, _ = baseline[key]
            if score > base_score:
                entry = (rel, line, f"{name}()  rose from {base_score} to {score}")
                (failures if score > FAIL_THRESHOLD else warnings).append(entry)
            elif score < base_score:
                lowered.append(
                    f"  {rel}:{line}  {name}()  fell from {base_score} to {score}")
        elif score > FAIL_THRESHOLD:
            failures.append(
                (rel, line, f"{name}()  new function scores {score}"))

    stale = sorted(k for k in baseline if k not in current)

    if lowered:
        print(f"{len(lowered)} function(s) improved - baseline left as-is, "
              "run --update-baseline to record the improvement:")
        for line in lowered:
            print(line)

    if stale and not args.changed:
        print(f"{len(stale)} baseline entries no longer matched (renamed or "
              "removed) - harmless, --update-baseline will drop them.")

    if warnings:
        print(f"\nWARNING: {len(warnings)} function(s) rose, still at or under "
              f"{FAIL_THRESHOLD}:")
        report(warnings, "warning")

    if failures:
        print(f"\nFAIL: {len(failures)} function(s) above {FAIL_THRESHOLD} "
              "grew or are new:")
        report(failures, "error")
        return 1

    print(f"\nPASS: {len(current)} function(s) checked, none above "
          f"{FAIL_THRESHOLD} grew.")
    return 0


def report(entries, level):
    """Print each entry, and as a GitHub annotation on that line in CI."""
    in_ci = os.environ.get("GITHUB_ACTIONS") == "true"
    for rel, line, text in entries:
        print(f"  {rel}:{line}  {text}")
        if in_ci:
            print(f"::{level} file={rel},line={line}::{text}")


if __name__ == "__main__":
    sys.exit(main())

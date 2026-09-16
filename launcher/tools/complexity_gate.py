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

The gate FAILS when a function's score rises above its last committed
baseline, a function absent from the baseline scores above
NEW_FUNCTION_THRESHOLD, or clang-tidy reports a parse error anywhere - a
partial parse can hide functions, so it is never accepted quietly. A
falling score PASSES with a note to lower the baseline - lowering it is
always an explicit --update-baseline, never automatic. See
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
# hand once already. A function with no baseline entry - new, or the
# renamed half of one - is judged against that same line.
NEW_FUNCTION_THRESHOLD = 15

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

VENDORED_DIR_NAMES = {"components", "managed_components"}

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
    pattern = str(Path.home() / ".espressif" / "tools" / "esp-clang" / "*" /
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
            "under ~/.espressif/tools/esp-clang/. This gate needs clang-tidy "
            f"{PINNED_MAJOR}.x - ESP-IDF's bundled esp-clang carries it "
            "(install ESP-IDF, or source its export script so PATH finds "
            "it), or install LLVM 19 directly "
            f"(apt install clang-tidy-{PINNED_MAJOR} / brew install llvm@{PINNED_MAJOR})."
        )
    candidate, major = fallback
    if not allow_any:
        sys.exit(
            f"Found clang-tidy {major} ({candidate}), but this gate pins "
            f"{PINNED_MAJOR}.x, the same major scripts/check-format.sh pins "
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
    glob-and-take-newest convention as the esp-clang lookup above; ESP-IDF
    installs both under the same ~/.espressif/tools/ prefix on every
    platform this project's CI or a contributor's machine runs on."""
    env = os.environ.get("XTENSA_GCC_ROOT")
    if env:
        return env
    pattern = str(Path.home() / ".espressif" / "tools" / "xtensa-esp-elf" /
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


def build_idf_entries(toolchain_root):
    """launcher/build.diag/compile_commands.json, restricted to this
    project's own main/ and test/ trees (excluding vendored code and
    apps/*/tools/, which the firmware never links), with the flags esp-idf
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
        if not parts or parts[0] not in ("main", "test"):
            continue
        if is_vendored(str(rel)) or "tools" in parts:
            continue
        cmd = inline_response_file(e.get("command", ""))
        cmd = " ".join(t for t in cmd.split() if t not in BAD_GCC_FLAGS)
        cmd += (f' --sysroot="{sysroot}" --gcc-toolchain="{toolchain_root}"'
                ' -Wno-unknown-warning-option -Wno-string-plus-int')
        entries[str(path.resolve())] = cmd
    return entries


# apps/*/tools/*.c - excluded from the firmware and the host build alike
# by long-standing convention (main/CMakeLists.txt, run_tests.sh), but the
# retired script's own usage text ("searched recursively for *.c") reached
# them when pointed at launcher/main, so parity means giving them real
# flags where that is possible at all: each already has its own working
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

    BUILD_DIR.mkdir(parents=True, exist_ok=True)
    db_json = []
    for path, cmd in all_entries.items():
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
    filesystem - independent of any build or database, the way the
    retired script's own recursive *.c search worked. A gap that is not
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
            "clang-tidy scanned {} file(s) and found ZERO functions. That is "
            "exactly how cognitive_complexity.py went blind - refusing to "
            "trust it. clang-tidy stderr:\n{}".format(len(files), proc.stderr[-4000:])
        )

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
    lowered = []
    for key, (score, line) in sorted(current.items(),
                                      key=lambda kv: (-kv[1][0], kv[0])):
        rel, name = key
        if key in baseline:
            base_score, _ = baseline[key]
            if score > base_score:
                failures.append(
                    f"  {rel}:{line}  {name}()  rose from {base_score} to {score}")
            elif score < base_score:
                lowered.append(
                    f"  {rel}:{line}  {name}()  fell from {base_score} to {score}")
        elif score > NEW_FUNCTION_THRESHOLD:
            failures.append(
                f"  {rel}:{line}  {name}()  new function scores {score} "
                f"(> {NEW_FUNCTION_THRESHOLD})")

    stale = sorted(k for k in baseline if k not in current)

    if lowered:
        print(f"{len(lowered)} function(s) improved - baseline left as-is, "
              "run --update-baseline to record the improvement:")
        for line in lowered:
            print(line)

    if stale and not args.changed:
        print(f"{len(stale)} baseline entries no longer matched (renamed or "
              "removed) - harmless, --update-baseline will drop them.")

    if failures:
        print(f"\nFAIL: {len(failures)} function(s) over their ratchet:")
        for line in failures:
            print(line)
        return 1

    print(f"\nPASS: {len(current)} function(s) checked, none rose above baseline.")
    return 0


if __name__ == "__main__":
    sys.exit(main())

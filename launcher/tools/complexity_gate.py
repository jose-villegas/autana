#!/usr/bin/env python3
"""
complexity_gate.py - a ratchet on clang-tidy's own
readability-function-cognitive-complexity check, not a fixed threshold.

Replaces cognitive_complexity.py, whose brace-alone-on-its-own-line regex
stopped matching anything the day PR #193 reformatted the tree, and so
reported "zero functions" - which reads exactly like a clean result.
clang-tidy parses a real AST; a reformat cannot blind it that way.

The gate FAILS when a function's score rises above its last committed
baseline, or a function absent from the baseline (new, or renamed) scores
above NEW_FUNCTION_THRESHOLD. A falling score PASSES with a note to lower
the baseline - lowering it is always an explicit --update-baseline, never
automatic. See docs/tools/Complexity-Gate.md for the full design notes,
the coverage accounting, and the threshold's reasoning.

USAGE
    complexity_gate.py                    the ratchet (default: --check)
    complexity_gate.py --update-baseline  overwrite the baseline with today's
                                           scores - a deliberate, human act
    complexity_gate.py --changed <ref>    scan only files git diff finds
                                           against <ref> (fast, local use)
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

DIAG_RE = re.compile(
    r"^(?P<file>.+):(?P<line>\d+):\d+: warning: function '(?P<name>[^']+)' "
    r"has cognitive complexity of (?P<score>\d+) \(threshold"
)


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
    """launcher/components/ (microui, small3dlib) and the vendored Unity
    framework under test/framework/ - third-party code, out of scope for a
    ratchet on THIS project's own functions."""
    parts = Path(path_str).parts
    if "components" in parts:
        return True
    if "framework" in parts and Path(path_str).name == "unity.c":
        return True
    return False


def build_compile_db():
    sources = [s for s in run_tests_print("--print-sources")
               if not is_vendored(s)]
    flags = run_tests_print("--print-flags")

    BUILD_DIR.mkdir(parents=True, exist_ok=True)
    entries = [
        {"directory": str(LAUNCHER_DIR), "file": src,
         "arguments": ["clang"] + flags + [src]}
        for src in sources
    ]
    db_path = BUILD_DIR / "compile_commands.json"
    db_path.write_text(json.dumps(entries, indent=2), encoding="utf-8")
    return db_path, sources


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

    db_path, all_sources = build_compile_db()
    print(f"compile database: {db_path} ({len(all_sources)} first-party host-compilable files)")

    files = all_sources
    if args.changed:
        files = changed_files(args.changed, all_sources)
        print(f"--changed {args.changed}: {len(files)} of those files touched")

    current, proc = scan(clang_tidy, db_path, files)

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

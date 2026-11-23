#!/bin/sh
#
# Runs every tooling test file in the repository, each in its own process,
# as many at once as there are CPUs. A tracked test_*.py runs as
# `python -m unittest discover -s <its dir> -p <its name>`, because -s is
# what puts that directory on sys.path, and each suite imports the module
# beside it. Directories with conftest.py or pytest.ini belong to pytest and
# are skipped. Each tracked test_*.mjs runs directly under `node --test`,
# since Node's runner needs no discovery step.
#
# Usage:
#   scripts/run-tool-tests.sh
#   scripts/run-tool-tests.sh --verbose
#   scripts/run-tool-tests.sh --jobs N       (default: the CPU count)
#
# The list comes from git on every run, so a test is found wherever it
# lives and build output is never searched. The files that took longest last
# run (run-tool-tests.times beside the log) start first, then the largest.
# Exits 1 if any file fails; keeps running the rest so one broken file does
# not hide another. A missing interpreter skips its language's files with a
# warning and still exits 1; unlike scripts/git-hooks/pre-commit, this
# script's job is to prove the suites pass, so a language it could not even
# run is a failure, not something to wave through quietly.
#
# Default output is the verdict and test count. A passing run can emit
# thousands of characters; the full stream, each file's output in one block
# in a fixed order, is in the printed log path.
#
# POSIX sh, plus xargs -P; .github/workflows/shell-scripts.yml parses it
# under dash.

set -eu

# shellcheck disable=SC1007
REPO_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
QUIET_LOG="$REPO_ROOT/launcher/test/build/run-tool-tests.log"
UNIT_DIR="$REPO_ROOT/launcher/test/build/run-tool-tests"
TIMES="$REPO_ROOT/launcher/test/build/run-tool-tests.times"

# One file, run by xargs below: its output, status and seconds go beside
# the others under UNIT_DIR, named by its path.
if [ "${1:-}" = --unit ]; then
    key=$(printf '%s' "$2" | tr '/' '_')
    started=$(date +%s)
    case "$2" in
        *.mjs) node --test "$REPO_ROOT/$2" ;;
        *) "$PYTHON" -u -m unittest discover -s "$REPO_ROOT/$(dirname -- "$2")" -p "$(basename -- "$2")" ;;
    esac >"$UNIT_DIR/$key.log" 2>&1 && rc=0 || rc=$?
    echo "$rc" >"$UNIT_DIR/$key.status"
    echo "$(($(date +%s) - started)) $2" >"$UNIT_DIR/$key.time"
    exit 0
fi

VERBOSE=0
JOBS=$(nproc 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1)
while [ $# -gt 0 ]; do
    case "$1" in
        --verbose) VERBOSE=1 ;;
        --jobs) [ $# -ge 2 ] || { echo "--jobs needs a count" >&2; exit 2; }; JOBS=$2; shift ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done

# shellcheck source=quiet.sh
. "$REPO_ROOT/scripts/quiet.sh"

# shellcheck source=lib/python.sh
. "$REPO_ROOT/scripts/lib/python.sh"
PYTHON=$(find_python 2>/dev/null) || PYTHON=""
export PYTHON UNIT_DIR

py_files=$(git -C "$REPO_ROOT" ls-files -- ':(glob)**/test_*.py' | sort)
mjs_files=$(git -C "$REPO_ROOT" ls-files -- ':(glob)**/test_*.mjs' | sort)

if [ -z "$py_files" ] && [ -z "$mjs_files" ]; then
    echo "git tracks no test_*.py or test_*.mjs anywhere in this repository." >&2
    exit 1
fi

quiet_begin "$QUIET_LOG"
# Passing tests print fixture text ("test_clip:FAIL:", argparse "error:"); only
# the runners' own failure headers and assertion lines are failures.
QUIET_ERROR_PATTERN='^FAIL: |^ERROR: |^AssertionError|^not ok |^# fail [1-9]'
export QUIET_STATUS

units=""
if [ -n "$py_files" ]; then
    if [ -z "$PYTHON" ]; then
        echo "python 3 was not found on PATH (tried python3, python, py); skipping test_*.py suites." >&2
        QUIET_STATUS=1
    else
        for file in $py_files; do
            dir=$(dirname -- "$file")
            [ ! -f "$REPO_ROOT/$dir/conftest.py" ] && [ ! -f "$REPO_ROOT/$dir/pytest.ini" ] || continue
            units="$units $file"
        done
    fi
fi
if [ -n "$mjs_files" ]; then
    if ! command -v node >/dev/null 2>&1; then
        echo "node was not found on PATH; skipping test_*.mjs suites." >&2
        QUIET_STATUS=1
    else
        units="$units $mjs_files"
    fi
fi

rm -rf "$UNIT_DIR"
mkdir -p "$UNIT_DIR"
[ -f "$TIMES" ] || : >"$TIMES"
# Longest first: last run's seconds, then bytes, which order a first run.
# shellcheck disable=SC2086
[ -z "$units" ] || (cd "$REPO_ROOT" && wc -c $units) |
    awk 'FILENAME == ARGV[1] { seconds[$2] = $1; next } $2 != "total" { print seconds[$2] + 0, $1, $2 }' "$TIMES" - |
    sort -k1,1nr -k2,2nr | cut -d' ' -f3 | xargs -P "$JOBS" -n 1 sh "$0" --unit

: >"$TIMES"
unit_count=0
for file in $units; do
    unit_count=$((unit_count + 1))
    key=$(printf '%s' "$file" | tr '/' '_')
    printf '== %s ==\n' "$file" >>"$QUIET_LOG"
    cat "$UNIT_DIR/$key.log" >>"$QUIET_LOG" 2>/dev/null || true
    cat "$UNIT_DIR/$key.time" >>"$TIMES" 2>/dev/null || true
    rc=$(cat "$UNIT_DIR/$key.status" 2>/dev/null || echo 1)
    if [ "$rc" -ne 0 ]; then
        QUIET_STATUS=$rc
        QUIET_FAILED_LABELS="${QUIET_FAILED_LABELS}${QUIET_FAILED_LABELS:+
}$file"
    fi
done
[ "$VERBOSE" -eq 0 ] || cat "$QUIET_LOG"

py_tests=$(grep -aoE '^Ran [0-9]+ tests? in' "$QUIET_LOG" | grep -oE '[0-9]+' | { sum=0; while read -r n; do sum=$((sum + n)); done; echo "$sum"; })
node_tests=$(grep -aoE 'tests [0-9]+$' "$QUIET_LOG" | grep -oE '[0-9]+' | { sum=0; while read -r n; do sum=$((sum + n)); done; echo "$sum"; })
QUIET_SUMMARY="$((py_tests + node_tests)) tests across $unit_count files on $JOBS jobs"
export QUIET_SUMMARY
quiet_end run-tool-tests

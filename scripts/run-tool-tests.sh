#!/bin/sh
#
# Runs every tooling test suite in the repository: each directory that git
# tracks a test_*.py in gets its own `python -m unittest discover -s <dir>`,
# because -s is what puts that directory on sys.path, and each suite imports
# the module beside it. Each tracked test_*.mjs runs directly under
# `node --test`, since Node's runner needs no discovery step.
#
# Usage:
#   scripts/run-tool-tests.sh
#   scripts/run-tool-tests.sh --verbose
#
# The list comes from git on every run, so a suite is found wherever it
# lives and build output is never searched. Exits 1 if any suite fails;
# keeps running the rest so one broken suite does not hide another. A
# missing interpreter skips its language's suites with a warning and still
# exits 1 - unlike scripts/git-hooks/pre-commit, this script's job is to
# prove the suites pass, so a language it could not even run is a failure,
# not something to wave through quietly.
#
# Default output is the verdict and test count. A passing run can emit
# thousands of characters; the full stream is in the printed log path.
#
# POSIX sh; .github/workflows/shell-scripts.yml parses it under dash.

set -eu

VERBOSE=0
while [ $# -gt 0 ]; do
    case "$1" in
        --verbose) VERBOSE=1 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done
export VERBOSE

# shellcheck disable=SC1007
REPO_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
QUIET_LOG="$REPO_ROOT/launcher/test/build/run-tool-tests.log"
# shellcheck source=quiet.sh
. "$REPO_ROOT/scripts/quiet.sh"

PYTHON=""
for candidate in python3 python; do
    if command -v "$candidate" >/dev/null 2>&1; then
        PYTHON="$candidate"
        break
    fi
done

py_dirs=$(git -C "$REPO_ROOT" ls-files -- ':(glob)**/test_*.py' | sed 's|/[^/]*$||' | sort -u)
mjs_files=$(git -C "$REPO_ROOT" ls-files -- ':(glob)**/test_*.mjs' | sort)

if [ -z "$py_dirs" ] && [ -z "$mjs_files" ]; then
    echo "git tracks no test_*.py or test_*.mjs anywhere in this repository." >&2
    exit 1
fi

suite_count=0
quiet_begin "$QUIET_LOG"
export QUIET_STATUS

if [ -n "$py_dirs" ]; then
    if [ -z "$PYTHON" ]; then
        echo "python 3 was not found on PATH (tried python3, python); skipping test_*.py suites." >&2
        QUIET_STATUS=1
    else
        for dir in $py_dirs; do
            suite_count=$((suite_count + 1))
            quiet_run "$dir" "$PYTHON" -m unittest discover -s "$REPO_ROOT/$dir" -p 'test_*.py' || true
        done
    fi
fi

if [ -n "$mjs_files" ]; then
    if ! command -v node >/dev/null 2>&1; then
        echo "node was not found on PATH; skipping test_*.mjs suites." >&2
        QUIET_STATUS=1
    else
        for file in $mjs_files; do
            suite_count=$((suite_count + 1))
            quiet_run "$file" node --test "$REPO_ROOT/$file" || true
        done
    fi
fi

py_tests=$(grep -oE '^Ran [0-9]+ tests? in' "$QUIET_LOG" | grep -oE '[0-9]+' | { sum=0; while read -r n; do sum=$((sum + n)); done; echo "$sum"; })
node_tests=$(grep -oE 'tests [0-9]+$' "$QUIET_LOG" | grep -oE '[0-9]+' | { sum=0; while read -r n; do sum=$((sum + n)); done; echo "$sum"; })
QUIET_SUMMARY="$((py_tests + node_tests)) tests across $suite_count suites"
export QUIET_SUMMARY
quiet_end run-tool-tests

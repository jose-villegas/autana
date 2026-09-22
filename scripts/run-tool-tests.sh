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
# Default output is the verdict and the test/suite counts, and on failure
# the failing suite(s) plus the assertion detail - each suite's own runner
# (unittest, node --test) prints a dot or a checkmark per test, which adds
# up to thousands of characters to say OK. The full stream goes to a log
# file whose path is always printed; --verbose streams everything instead,
# as this script always used to.
#
# POSIX sh; .github/workflows/shell-scripts.yml parses it under dash.

set -eu

VERBOSE=0
if [ "${1:-}" = "--verbose" ]; then
    VERBOSE=1
    shift
fi

REPO_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
QUIET_LOG="$REPO_ROOT/scripts/run-tool-tests.log"

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

status=0
suite_count=0
failed_labels=""

[ "$VERBOSE" -eq 1 ] || : >"$QUIET_LOG"

# run_one <label> <command...> - runs one suite, quiet by default (output
# appended to $QUIET_LOG, a "== label ==" marker either way so the log reads
# like the stream this replaces), streamed live under --verbose.
run_one() {
    label="$1"
    shift
    suite_count=$((suite_count + 1))
    if [ "$VERBOSE" -eq 1 ]; then
        echo "== $label =="
        if ! "$@"; then
            echo "FAILED: $label" >&2
            status=1
        fi
        return 0
    fi
    echo "== $label ==" >>"$QUIET_LOG"
    if ! "$@" >>"$QUIET_LOG" 2>&1; then
        echo "FAILED: $label" >>"$QUIET_LOG"
        status=1
        failed_labels="$failed_labels $label"
    fi
}

if [ -n "$py_dirs" ]; then
    if [ -z "$PYTHON" ]; then
        echo "python 3 was not found on PATH (tried python3, python); skipping test_*.py suites." >&2
        status=1
    else
        for dir in $py_dirs; do
            run_one "$dir" "$PYTHON" -m unittest discover -s "$REPO_ROOT/$dir" -p 'test_*.py'
        done
    fi
fi

if [ -n "$mjs_files" ]; then
    if ! command -v node >/dev/null 2>&1; then
        echo "node was not found on PATH; skipping test_*.mjs suites." >&2
        status=1
    else
        for file in $mjs_files; do
            run_one "$file" node --test "$REPO_ROOT/$file"
        done
    fi
fi

if [ "$VERBOSE" -eq 0 ]; then
    py_tests=$(grep -oE '^Ran [0-9]+ tests? in' "$QUIET_LOG" | grep -oE '[0-9]+' | { sum=0; while read -r n; do sum=$((sum + n)); done; echo "$sum"; })
    node_tests=$(grep -oE 'tests [0-9]+$' "$QUIET_LOG" | grep -oE '[0-9]+' | { sum=0; while read -r n; do sum=$((sum + n)); done; echo "$sum"; })
    total_tests=$((py_tests + node_tests))

    if [ "$status" -eq 0 ]; then
        echo "ok run-tool-tests: $total_tests tests across $suite_count suites"
    else
        echo "FAIL run-tool-tests: $total_tests tests across $suite_count suites"
        echo "  failing:$failed_labels"
        grep -E '^(FAIL|ERROR): |^AssertionError|^ *File "|^. [A-Za-z].*\([0-9.]+ ?ms\)$|^not ok |Error[:[]|undefined reference to' "$QUIET_LOG" |
            head -n 60 | sed 's/^/  /'
    fi
    echo "full log: $QUIET_LOG"
fi

exit "$status"

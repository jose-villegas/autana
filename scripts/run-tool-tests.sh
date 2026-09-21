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
#
# The list comes from git on every run, so a suite is found wherever it
# lives and build output is never searched. Exits 1 if any suite fails;
# keeps running the rest so one broken suite does not hide another. A
# missing interpreter skips its language's suites with a warning instead of
# failing outright, matching scripts/git-hooks/pre-commit's style.
#
# POSIX sh; .github/workflows/shell-scripts.yml parses it under dash.

set -eu

REPO_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

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

if [ -n "$py_dirs" ]; then
    if [ -z "$PYTHON" ]; then
        echo "python 3 was not found on PATH (tried python3, python); skipping test_*.py suites." >&2
        status=1
    else
        for dir in $py_dirs; do
            echo "== $dir =="
            if ! "$PYTHON" -m unittest discover -s "$REPO_ROOT/$dir" -p 'test_*.py'; then
                echo "FAILED: $dir" >&2
                status=1
            fi
        done
    fi
fi

if [ -n "$mjs_files" ]; then
    if ! command -v node >/dev/null 2>&1; then
        echo "node was not found on PATH; skipping test_*.mjs suites." >&2
        status=1
    else
        for file in $mjs_files; do
            echo "== $file =="
            if ! node --test "$REPO_ROOT/$file"; then
                echo "FAILED: $file" >&2
                status=1
            fi
        done
    fi
fi

exit "$status"

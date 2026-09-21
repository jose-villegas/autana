#!/bin/sh
#
# Runs every tooling test suite in the repository: each directory that git
# tracks a test_*.py in gets its own `python -m unittest discover -s <dir>`,
# because -s is what puts that directory on sys.path, and each suite imports
# the module beside it.
#
# Usage:
#   scripts/run-tool-tests.sh
#
# The list comes from git on every run, so a suite is found wherever it
# lives and build output is never searched. Exits 1 if any suite fails;
# keeps running the rest so one broken suite does not hide another.
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
if [ -z "$PYTHON" ]; then
    echo "python 3 was not found on PATH (tried python3, python)." >&2
    exit 1
fi

dirs=$(git -C "$REPO_ROOT" ls-files -- ':(glob)**/test_*.py' | sed 's|/[^/]*$||' | sort -u)
if [ -z "$dirs" ]; then
    echo "git tracks no test_*.py anywhere in this repository." >&2
    exit 1
fi

status=0
for dir in $dirs; do
    echo "== $dir =="
    if ! "$PYTHON" -m unittest discover -s "$REPO_ROOT/$dir" -p 'test_*.py'; then
        echo "FAILED: $dir" >&2
        status=1
    fi
done

exit "$status"

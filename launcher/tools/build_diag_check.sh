#!/usr/bin/env bash
#
# Build the DIAGNOSTICS image and run the complexity ratchet - no device,
# nothing flashed.
#
#   tools/build_diag_check.sh [IDF_EXPORT]
#
# Exists because the diagnostics variant is the only one that links every
# test suite into firmware, so it is the only one where a suite's own
# static data is charged against the same internal-heap headroom the shell
# and its apps need. A suite that costs 10 KiB of .bss is invisible to the
# host runner (which has a laptop's memory behind it) and to a release
# build (which links no suites at all) - building this variant surfaces it,
# and doing so here beats finding out from CI.
#
# The complexity ratchet (tools/complexity_gate.py) is the other half of
# what CI's Build (Diagnostics) workflow decides, so a green build alone
# settles nothing about a pull request - both halves are this one command.
# The ratchet runs BEFORE the build: it costs seconds, the build minutes.
#
# The ratchet reads launcher/build.diag/compile_commands.json, which this
# build writes, so it can only run first once a build.diag exists - with no
# database on disk it runs after the build instead. That database is also
# the previous build's: a .c file added since then is unknown to it and the
# ratchet fails naming that file as unmeasured, which a build clears.
#
# COMPLEXITY_GATE_BASE overrides the ref the ratchet diffs against
# (default origin/main, the same ref docs/tools/Complexity-Gate.md names
# for local use; CI scores the whole tree instead).
#
# See build_flash.sh for the arguments and for why both -D flags on its
# idf.py call are load-bearing.

set -euo pipefail

DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
COMPILE_DB="$DIR/../build.diag/compile_commands.json"
GATE_BASE="${COMPLEXITY_GATE_BASE:-origin/main}"

PYTHON=$(command -v python3 || command -v python || true)
if [ -z "$PYTHON" ]; then
    echo "python 3 was not found on PATH (tried python3, python)." >&2
    exit 1
fi

complexity_gate() {
    echo "=== Complexity ratchet (--changed $GATE_BASE) ==="
    (cd "$REPO_ROOT" && "$PYTHON" launcher/tools/complexity_gate.py \
        --changed "$GATE_BASE")
}

if [ -f "$COMPILE_DB" ]; then
    complexity_gate
    exec "$DIR/build_flash.sh" --diag --build-only "$@"
fi

echo "=== No build.diag compile database yet - ratchet runs after the build ==="
"$DIR/build_flash.sh" --diag --build-only "$@"
complexity_gate

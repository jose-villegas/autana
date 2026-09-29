#!/usr/bin/env bash
#
# Build the DIAGNOSTICS image and run the complexity ratchet - no device,
# nothing flashed.
#
#   tools/build/build_diag_check.sh
#
# The result and the log path are printed; the log holds the full output.
#
# Exists because the diagnostics variant is the only one that links every
# test suite into firmware, so it is the only one where a suite's own
# static data is charged against the same internal-heap headroom the shell
# and its apps need. A suite that costs 10 KiB of .bss is invisible to the
# host runner (which has a laptop's memory behind it) and to a release
# build (which links no suites at all) - building this variant surfaces it,
# and doing so here beats finding out from CI.
#
# The complexity ratchet (tools/quality/complexity_gate.py) is the other half of
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
# The build is `autana build diag`, which runs build.sh; ESP-IDF is the one
# under $IDF_PATH.

set -euo pipefail

case "${1:-}" in
    "") ;;
    -h|--help) echo "usage: tools/build/build_diag_check.sh"; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
esac

# shellcheck disable=SC1007
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
# shellcheck disable=SC1007
REPO_ROOT=$(CDPATH= cd -- "$DIR/../../.." && pwd)
COMPILE_DB="$DIR/../../build.diag/compile_commands.json"
GATE_BASE="${COMPLEXITY_GATE_BASE:-origin/main}"
# shellcheck source=../../../scripts/quiet.sh
. "$DIR/../../../scripts/quiet.sh"

if [ -z "${QUIET_INNER:-}" ]; then
    quiet_begin "$DIR/../../build.diag/build_diag_check.log"
    quiet_run diagnostics-check env QUIET_INNER=1 bash "$0" || true
    quiet_end build_diag_check || exit $?
    exit 0
fi

PYTHON=$(command -v python3 || command -v python || true)
if [ -z "$PYTHON" ]; then
    echo "python 3 was not found on PATH (tried python3, python)." >&2
    exit 1
fi

complexity_gate() {
    echo "=== Complexity ratchet (--changed $GATE_BASE) ==="
    (cd "$REPO_ROOT" && "$PYTHON" launcher/tools/quality/complexity_gate.py \
        --changed "$GATE_BASE")
}

suite_static_data_gate() {
    # The directory goes in as its own argument so Git Bash converts it for a
    # Windows Python; inside the -c string it would stay a /c/... path.
    IDF_PYTHON=$("$PYTHON" -c "import sys; sys.path.insert(0, sys.argv[1]); from espressif import idf_python; print(idf_python())" "$DIR")
    "$IDF_PYTHON" "$IDF_PATH/tools/idf_size.py" --files --format csv "$DIR/../../build.diag/launcher.map" |
        "$PYTHON" "$DIR/../quality/suite_static_data_gate.py"
}

build_diag() {
    "$REPO_ROOT/tools/autana" build diag --project "$REPO_ROOT"
}

if [ -f "$COMPILE_DB" ]; then
    complexity_gate
    build_diag
    suite_static_data_gate
    exit 0
fi

echo "=== No build.diag compile database yet - ratchet runs after the build ==="
build_diag
complexity_gate
suite_static_data_gate

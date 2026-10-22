#!/bin/bash
# Runs one held nightly lane: board plan data out -- suite arguments.
set -uo pipefail
board=$1
plan=$2
data=$3
out=$4
shift 4
[ "${1:-}" = -- ] || { echo "expected -- suite arguments" >&2; exit 2; }
shift
repo=$(pwd)
. scripts/lib/python.sh
PY=$(find_python) || exit 1
driver=launcher/tools/perf/nightly/scoped_chain.py
info=$("$PY" "$driver" lane-info --plan "$plan" --board "$board") || exit 1
read -r lane estimate <<< "$info"
wait_seconds=$("$PY" "$driver" wait-seconds --until "$WINDOW_END") || exit 1
args=(run --plan "$plan" --repo "$repo" --data "$data" --out "$out/$board"
      --board "$board" --lane "$lane" --until "$WINDOW_END" "$@")
unset BOARD_CI_LOG
if [ "$board" = "$SELFTEST_BOARD" ]; then
    export BOARD_CI_LOG="$out/$board-selftest.log"
    shell=$BASH
    if command -v cygpath >/dev/null 2>&1; then
        shell=$(cygpath -w "$shell")
        BOARD_CI_LOG=$(cygpath -u "$BOARD_CI_LOG")
    fi
    args+=(--selftest "$shell" "$repo/scripts/ci/board-steps.sh")
fi
autana --board "$board" --wait "$wait_seconds" lock run --estimate "$estimate" -- "$PY" "$driver" "${args[@]}"

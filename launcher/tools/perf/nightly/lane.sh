#!/bin/bash
set -uo pipefail
board=$1
plan=$(cygpath -u "$2")
data=$(cygpath -u "$3")
out=$(cygpath -u "$4")
repo=$(pwd)
. scripts/lib/python.sh
PY=$(find_python) || exit 1
driver=launcher/tools/perf/nightly/scoped_chain.py
read -r lane estimate < <("$PY" -c 'import json,sys; p=json.load(open(sys.argv[1]))["lanes"][sys.argv[2]]; print(p["lane"], p["estimate"])' "$plan" "$board")
wait_seconds=$("$PY" "$driver" wait-seconds --until "$WINDOW_END") || exit 1
args=(run --plan "$plan" --repo "$repo" --data "$data" --out "$out/$board"
      --board "$board" --lane "$lane" --until "$WINDOW_END"
      --suite run_sand_perf_suite - 'python3 launcher/main/apps/sand/tools/report_performance.py @CAPTURE@ @TABLE@ --source @PROJECT@/launcher/main/apps/sand/tests/suite_sand_perf.c'
      --suite run_sponza_perf_suite - -
      --suite run_capybara_perf_suite - 'python3 launcher/main/apps/render_lab/tools/report_performance.py @CAPTURE@ @TABLE@ --table')
if [ "$board" = "$SELFTEST_BOARD" ]; then
    export BOARD_CI_LOG="$out/$board-selftest.log"
    args+=(--selftest-at "$SELFTEST_AT" --selftest "$(cygpath -w "$BASH")" "$repo/scripts/ci/board-steps.sh")
fi
autana --board "$board" --wait "$wait_seconds" lock run --estimate "$estimate" -- "$PY" "$driver" "${args[@]}"

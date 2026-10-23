#!/bin/sh
# Board work must finish before joining the heavy queue; recheck the cap after it.
set -eu
: "${GPU_WORKFLOW:?GPU_WORKFLOW is required}"
status=0
python3 scripts/lib/local_schedule.py ready || status=$?
skipped=$(python3 -c 'from scripts.lib.local_schedule import EXIT_SKIPPED; print(EXIT_SKIPPED)')
if [ "$status" -eq "$skipped" ]; then exit 0; fi
[ "$status" -eq 0 ] || exit "$status"
wait_seconds=$(python3 scripts/lib/local_schedule.py budget)
export PATH="$PWD/tools:$PATH"
status=0
autana lock run --heavy --wait "$wait_seconds" -- \
    python3 scripts/lib/local_schedule.py start -- "$@" || status=$?
busy=$(python3 -c 'from scripts.lib.autana_config import EXIT_BUSY; print(EXIT_BUSY)')
if [ "$status" -eq "$busy" ] && [ "${OVERRIDE:-}" != true ]; then
    if [ "${GITHUB_EVENT_NAME:-}" = schedule ] || [ "${GITHUB_EVENT_NAME:-}" = workflow_run ]; then
        python3 -c 'from scripts.lib.local_schedule import SKIPPED; print(SKIPPED)'
        exit 0
    fi
    python3 -c 'from scripts.lib.local_schedule import REFUSED; print(REFUSED)'
fi
exit "$status"

#!/bin/sh
# Board work finishes before joining the heavy queue.
set -eu
: "${GPU_WORKFLOW:?GPU_WORKFLOW is required}"
status=0
python3 scripts/lib/local_schedule.py ready || status=$?
deferred=$(python3 -c 'from scripts.lib.local_schedule import EXIT_DEFERRED; print(EXIT_DEFERRED)')
if [ "$status" -eq "$deferred" ]; then exit 0; fi
[ "$status" -eq 0 ] || exit "$status"
hang_seconds=$(python3 -c 'from scripts.lib.local_schedule import NIGHT_STEP_HANG_SECONDS; print(NIGHT_STEP_HANG_SECONDS)')
export PATH="$PWD/tools:$PATH"
autana lock run --heavy --wait "$hang_seconds" -- \
    python3 scripts/lib/local_schedule.py start -- "$@"

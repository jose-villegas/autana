#!/bin/sh
# A quiet-host stage waits for board work, then holds heavy; any other runs beside it on its CPU share.
set -eu
: "${GPU_WORKFLOW:?GPU_WORKFLOW is required}"
status=0
python3 scripts/lib/local_schedule.py ready || status=$?
deferred=$(python3 -c 'from scripts.lib.local_schedule import EXIT_DEFERRED; print(EXIT_DEFERRED)')
if [ "$status" -eq "$deferred" ]; then exit 0; fi
[ "$status" -eq 0 ] || exit "$status"
if ! python3 -c 'import os, sys; from scripts.lib.local_schedule import quiet_host; sys.exit(not quiet_host(os.environ["GPU_WORKFLOW"]))'; then
    exec python3 scripts/lib/local_schedule.py start -- "$@"
fi
hang_seconds=$(python3 -c 'from scripts.lib.local_schedule import NIGHT_STEP_HANG_SECONDS; print(NIGHT_STEP_HANG_SECONDS)')
export PATH="$PWD/tools:$PATH"
autana lock run --heavy --wait "$hang_seconds" -- \
    python3 scripts/lib/local_schedule.py start -- "$@"

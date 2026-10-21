#!/bin/sh
# Recheck the start cap after the FIFO wait, before running the full stage.
set -eu
wait_seconds=$(python3 - <<'PY'
import os
from scripts.lib.local_schedule import Clock, GPU_END, wait_seconds
from scripts.lib.autana_config import DEFAULT_LOCK_WAIT_SECONDS
print(DEFAULT_LOCK_WAIT_SECONDS if os.environ.get("OVERRIDE") == "true"
      else wait_seconds(GPU_END, Clock().now()))
PY
)
export PATH="$PWD/tools:$PATH"
status=0
autana lock run --heavy --wait "$wait_seconds" -- \
    python3 scripts/lib/local_schedule.py start -- "$@" || status=$?
busy=$(python3 -c 'from scripts.lib.autana_config import EXIT_BUSY; print(EXIT_BUSY)')
if [ "$status" -eq "$busy" ] && [ "${OVERRIDE:-}" != true ]; then
    python3 -c 'from scripts.lib.local_schedule import SKIPPED; print(SKIPPED)'
    exit 0
fi
exit "$status"

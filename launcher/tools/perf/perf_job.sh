#!/bin/sh
# Sourced by perf_compare.sh and perf_sweep.sh, expects the variables PYTHON, TOOLS_DIR, autana (the command) and wait (seconds).
# A process-bound hold keeps restoration inside the job even after an interrupt.
. "$TOOLS_DIR/../../../scripts/lib/process_pid.sh"

perf_job_autana() {
    "$PYTHON" -c 'import sys, subprocess
sys.path.insert(0, sys.argv[1])
from layout_measure import autana_command
sys.exit(subprocess.call(autana_command(*sys.argv[3:], override=sys.argv[2])))' \
        "$TOOLS_DIR" "$autana" "$@"
}

perf_job_take() {
    perf_job_token=""
    if [ -z "${_AUTANA_DEVICE_LOCK_TOKEN:-}" ]; then
        perf_job_pid=$(host_pid "$$")
        perf_job_token=$(perf_job_autana --wait "$wait" lock take --until-released --pid "$perf_job_pid" \
            --estimate "$1" "$2")
        _AUTANA_DEVICE_LOCK_TOKEN=$perf_job_token
        export _AUTANA_DEVICE_LOCK_TOKEN
    fi
}

perf_job_release() {
    if [ -n "$perf_job_token" ]; then
        perf_job_autana lock release "$perf_job_token"
    fi
}

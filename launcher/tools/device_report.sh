#!/bin/sh
#
# The one build-flash-capture-report path. Source this and call
# device_report_run "$@"; everything that differs between reports is a
# variable or a small function the caller declares first, so a report script
# holds declarations and no procedure of its own.
#
# Every report script shares this one procedure rather than carrying its
# own copy of the build flags and the capture call - a second copy
# drifting out of sync from this one would surface as a capture that
# measures nothing rather than as an error. A second copy of this
# procedure is the bug; do not write one.
#
# WHAT A CALLER DECLARES
#
#   report_name        file-name stem for the report and its raw capture
#   report_dir         directory both land in (created if absent)
#   report_timeout     seconds to capture for
#   report_suite       "" for a boot-time run of every compiled-in suite;
#                      a registered suite name to send RUNSUITE instead.
#                      This one choice decides the build too: a boot-time
#                      run needs the suites started before the shell, a
#                      RUNSUITE run needs the shell up to listen, so the
#                      image can never be the wrong one for the capture.
#   report_build_flags extra device.py selftest flags, e.g. --perf-scope
#   report_sentinel    a line the capture must contain to count as having
#                      measured anything, beyond the results every run prints
#   report_failures_ok 1 if the reporter exits 1 to mean "the report records
#                      a failing test", which is a result to read and not a
#                      failure of this run
#   report_generate()  function: run the python reporter, "$1" raw -> "$2" md
#   report_summary()   optional function, same arguments: print whatever is
#                      worth reading without opening the report
#
# and then calls device_report_run with the arguments every report takes:
#
#   [--no-restore] [COM_PORT] [OUT.md]

device_report_run() {
    # Defaults, applied here rather than at source time: a caller declares
    # before it sources, and a top-level assignment would blank what it said.
    : "${report_build_flags:=}"
    : "${report_sentinel:=}"
    : "${report_failures_ok:=0}"
    for _dr_required in report_name report_dir report_timeout report_suite; do
        eval "_dr_value=\${$_dr_required+set}"
        if [ -z "${_dr_value:-}" ]; then
            echo "ERROR: $_dr_required was never declared - see tools/device_report.sh" >&2
            return 1
        fi
    done
    if ! command -v report_generate > /dev/null 2>&1; then
        echo "ERROR: report_generate() was never declared - see tools/device_report.sh" >&2
        return 1
    fi

    _dr_restore=1
    while [ $# -gt 0 ]; do
        case "$1" in
            --no-restore) _dr_restore=0; shift ;;
            -*) echo "ERROR: unknown flag: $1" >&2; return 1 ;;
            *) break ;;
        esac
    done

    _dr_port="${1:-}"
    _dr_out="${2:-}"

    # launcher/, wherever this report lives: beside tools/build_flash.sh, or
    # four folders down in an app's own tools/. Found by walking up to the
    # folder that holds this file rather than by counting levels, so moving a
    # report script between the two is not a second thing to edit. Not by
    # CMakeLists.txt: main/ and the components each have one of those too.
    _dr_here="$(cd "$(dirname "$0")" && pwd)"
    _dr_launcher="$_dr_here"
    while [ ! -f "$_dr_launcher/tools/device_report.sh" ] && [ "$_dr_launcher" != "/" ]; do
        _dr_launcher="$(dirname "$_dr_launcher")"
    done
    if [ ! -f "$_dr_launcher/tools/device_report.sh" ]; then
        echo "ERROR: no launcher/tools/ above $_dr_here" >&2
        return 1
    fi
    _dr_tools="$_dr_launcher/tools"
    _dr_worktree="$(cd "$_dr_launcher/.." && pwd)"
    _dr_device_py="$_dr_worktree/scripts/device/device.py"
    _dr_owner="${AUTANA_DEVICE_OWNER:-device_report}"

    mkdir -p "$report_dir"
    _dr_stamp="$(date +%Y%m%d_%H%M%S)"
    if [ -z "$_dr_out" ]; then
        _dr_out="$report_dir/${report_name}_$_dr_stamp.md"
    fi
    _dr_raw="$report_dir/${report_name}_${_dr_stamp}_raw.txt"

    _dr_do_restore="$_dr_restore"
    trap device_report_finish EXIT

    device_report_capture || return 1
    device_report_validate || return 1
    device_report_report || return 1

    echo "=== Report:       $_dr_out ==="
    echo "=== Raw capture:  $_dr_raw ==="
    if command -v report_summary > /dev/null 2>&1; then
        report_summary "$_dr_raw" "$_dr_out"
    fi
}

# Build+flash and capture are one held-lock call, scripts/device/device.py's
# own `selftest` (report_suite="", every suite at boot) or `batch --runs 1`
# (report_suite=<name>, one suite via RUNSUITE - the same build-then-
# capture-under-one-lock shape, scoped to a single suite and run). COM_PORT,
# when given, is passed through; otherwise device.py finds the board by its
# USB identity.
device_report_capture() {
    if [ -n "$report_suite" ]; then
        set -- --owner "$_dr_owner" batch --worktree "$_dr_worktree" --suite "$report_suite" \
               --runs 1 --out "$_dr_raw" --max-seconds "$report_timeout" \
               --purpose "device_report $report_name"
        echo "=== Building and capturing RUNSUITE $report_suite ==="
    else
        set -- --owner "$_dr_owner" selftest --worktree "$_dr_worktree" --out "$_dr_raw" \
               --max-seconds "$report_timeout" --purpose "device_report $report_name"
        echo "=== Building and capturing the self-test run ==="
    fi
    if [ -n "$_dr_port" ]; then
        set -- --port "$_dr_port" "$@"
    fi
    # Unquoted on purpose: a caller declares zero or more flags in one string.
    # shellcheck disable=SC2086
    set -- "$@" $report_build_flags
    python "$_dr_device_py" "$@"
}

# A capture that never finished, crashed, or measured nothing still produces
# a plausible-looking report if fed straight to a reporter - that has cost
# two full capture cycles and once got mistaken for a fresh result.
device_report_validate() {
    echo "=== Validating capture ==="
    set -- "$_dr_raw"
    # A RUNSUITE capture ends when its window does, not with the line a
    # boot-time run prints, so requiring that line would reject every one.
    if [ -n "$report_suite" ]; then
        set -- "$@" --no-complete
    fi
    if [ -n "$report_sentinel" ]; then
        set -- "$@" --sentinel "$report_sentinel"
    fi
    python "$_dr_tools/sweeps/validate_capture.py" "$@"
}

device_report_report() {
    echo "=== Generating report ==="
    _dr_status=0
    report_generate "$_dr_raw" "$_dr_out" || _dr_status=$?
    if [ "$_dr_status" -eq 0 ]; then
        return 0
    fi
    # Exit 1 from a reporter means the report itself records a failing test,
    # which is a normal outcome to read rather than a failure of this run.
    # Anything else - a missing file, a capture the reporter refuses - is
    # fatal, and a blanket `|| true` used to swallow it along with the rest.
    if [ "$_dr_status" -eq 1 ] && [ "$report_failures_ok" -eq 1 ]; then
        echo "=== The report records failing tests - read it; this exit code is not the verdict ==="
        return 0
    fi
    echo "ERROR: the reporter exited $_dr_status - it wrote no report, or it" >&2
    echo "refused the capture. Its own message above says which." >&2
    return 1
}

device_report_finish() {
    _dr_final=$?
    if [ "$_dr_do_restore" -eq 1 ]; then
        echo "=== Restoring the release firmware ==="
        set -- --owner "$_dr_owner" flash --variant release --worktree "$_dr_worktree" \
               --purpose "device_report $report_name (restore)"
        if [ -n "$_dr_port" ]; then
            set -- --port "$_dr_port" "$@"
        fi
        python "$_dr_device_py" "$@" \
            || echo "WARNING: could not restore the release firmware - the device may still be on build.diag"
    else
        echo "=== --no-restore: leaving the device on the diagnostics image ==="
        echo "Reminder: the device is still running the diagnostics image, not"
        echo "the release firmware. Run this script once without --no-restore,"
        echo "or flash release yourself, before treating the board as normal."
    fi
    if [ "$_dr_final" -ne 0 ]; then
        echo
        echo "=== FAILED (exit $_dr_final) ==="
    fi
    # The pause is a convenience for a double-clicked window, which always
    # has a terminal. A piped or redirected run has none, and reading from
    # one turned a clean run into a reported failure back when its status
    # decided the script's.
    if [ -t 0 ]; then
        printf 'Press Enter to close...'
        read -r _dr_dismissed || true
    fi
    exit "$_dr_final"
}

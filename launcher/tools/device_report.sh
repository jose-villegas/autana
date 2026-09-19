#!/bin/sh
#
# The one build-flash-capture-report path. Source this and call
# device_report_run "$@"; everything that differs between reports is a
# variable or a small function the caller declares first, so a report script
# holds declarations and no procedure of its own.
#
# WHY THIS EXISTS
#
# There were four scripts doing this job, each carrying its own copy of the
# sdkconfig fragment list, the stale-config removal, the flag assertion and
# the capture call. Two of them diverged on a build flag twice - SDKCONFIG
# once, CONFIG_LAUNCHER_SELFTEST_AUTORUN once - and both times the symptom
# was a capture that measured nothing rather than an error. A second copy of
# this procedure is the bug; do not write one.
#
# The build half is not here either: build_flash.sh owns which fragments a
# request layers, which flags must be present or absent afterwards, and
# deleting a build directory's sdkconfig when it disagrees with the request.
# This calls it.
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
#   report_build_flags extra build_flash.sh flags, e.g. --perf-scope
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
#   [--no-restore] [COM_PORT] [OUT.md] [IDF_EXPORT]

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
    _dr_export="${3:-}"

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

    # shellcheck source=./find_port.sh
    . "$_dr_tools/find_port.sh"
    if [ -z "$_dr_port" ]; then
        _dr_port="$(find_port)" || _dr_port=""
    fi
    device_report_check_port "$_dr_port" || return 1

    mkdir -p "$report_dir"
    _dr_stamp="$(date +%Y%m%d_%H%M%S)"
    if [ -z "$_dr_out" ]; then
        _dr_out="$report_dir/${report_name}_$_dr_stamp.md"
    fi
    _dr_raw="$report_dir/${report_name}_${_dr_stamp}_raw.txt"

    # Installed only once the board is about to be written to: nothing needs
    # restoring before that, and a wrong port should not cost a release build.
    _dr_do_restore="$_dr_restore"
    trap device_report_finish EXIT

    device_report_build || return 1
    device_report_capture || return 1
    device_report_validate || return 1
    device_report_report || return 1

    echo "=== Report:       $_dr_out ==="
    echo "=== Raw capture:  $_dr_raw ==="
    if command -v report_summary > /dev/null 2>&1; then
        report_summary "$_dr_raw" "$_dr_out"
    fi
}

# Fail in seconds, not in twelve minutes: a vanished port used to be
# discovered by esptool only AFTER a full cold build. Enumerating the
# registry never opens the port, and neither does testing for a device node.
device_report_check_port() {
    if [ -z "$1" ]; then
        echo "ERROR: no serial port given, and none was found." >&2
        echo "Plug in the device, or pass the port as the first argument." >&2
        return 1
    fi
    if [ -n "${MSYSTEM:-}" ]; then
        _dr_seen="$(powershell.exe -NoProfile -ExecutionPolicy Bypass -Command \
            "[System.IO.Ports.SerialPort]::GetPortNames() -join ','" 2>&1 | tr -d '\r\n')"
        case ",$_dr_seen," in
            *",$1,"*) return 0 ;;
        esac
        echo "ERROR: $1 not found." >&2
        echo "Ports currently visible to Windows: ${_dr_seen:-(none)}" >&2
        echo "Plug in the device, or pass the right port as the first argument." >&2
        return 1
    fi
    if [ ! -e "$1" ]; then
        echo "ERROR: $1 does not exist." >&2
        return 1
    fi
    return 0
}

device_report_build() {
    echo "=== Building and flashing the diagnostics image to $_dr_port ==="
    set -- --diag
    if [ -z "$report_suite" ]; then
        set -- "$@" --autorun
    fi
    # Unquoted on purpose: a caller declares zero or more flags in one string.
    # shellcheck disable=SC2086
    set -- "$@" $report_build_flags "$_dr_port"
    if [ -n "$_dr_export" ]; then
        set -- "$@" "$_dr_export"
    fi
    # No stdin: build_flash.sh pauses for a double-clicker at the end, and
    # this is not one.
    sh "$_dr_tools/build_flash.sh" "$@" < /dev/null
}

device_report_capture() {
    if [ -z "$report_suite" ]; then
        echo "=== Capturing the self-test run ==="
        python "$_dr_tools/sweeps/capture_selftest.py" "$_dr_raw" \
            --port "$_dr_port" --timeout "$report_timeout"
    else
        echo "=== Triggering RUNSUITE $report_suite and capturing output ==="
        python "$_dr_tools/sweeps/capture_runsuite.py" "$report_suite" "$_dr_raw" \
            --port "$_dr_port" --timeout "$report_timeout"
    fi
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
        set --
        if [ -n "$_dr_port" ]; then
            set -- "$_dr_port"
        fi
        if [ -n "$_dr_export" ]; then
            set -- "$@" "$_dr_export"
        fi
        sh "$_dr_tools/build_flash.sh" "$@" < /dev/null \
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

#!/bin/sh
#
# One-click device performance report: build+flash the diagnostics image,
# capture the boot-time run, write a markdown table of just the frame-budget
# tests (scenario, budget, measured, headroom, pass/fail) - generated fresh
# from a real capture and the current source, so it can never go stale the
# way a hand-transcribed copy can.
#
# Usage:
#   main/apps/sand/tools/report_performance.sh [--no-restore] [--perf-scope] \
#       [--baseline REPORT.md] [COM_PORT] [OUT.md]
#
#   COM_PORT     serial port the device is on. Found by USB identity when
#                omitted - see scripts/device/device.py.
#   OUT.md       markdown report path. Default:
#                main/apps/sand/tools/results/performance_<timestamp>.md
#   --no-restore skip rebuilding and reflashing the release firmware
#                afterward. Restoring costs a build+flash on EVERY run;
#                back-to-back candidate captures only need it once, at the
#                end of a session.
#   --perf-scope build the PERF-SCOPED image: only suite_sand_perf and the
#                scene builders it calls are compiled in, so the run is
#                shorter and static RAM is freed up. Its numbers are NOT
#                comparable with an unscoped capture's - the layout differs -
#                so scope every capture of a round the same way.
#   --baseline REPORT.md
#                after generating the report, run compare_reports.py
#                --verdict against this earlier report and print its verdict.
#
# Everything this does beyond the declarations below - which image, deleting
# a build directory's sdkconfig that disagrees, asserting the flags took,
# capturing, validating, restoring release - is tools/device_report.sh.

set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
# tools -> sand -> apps -> main -> launcher.
LAUNCHER_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/../../../.." && pwd)

report_name=performance
# The app's own results dir: deleting main/apps/sand/ takes its scratch
# output with it too.
report_dir="$SCRIPT_DIR/results"
report_suite=""

# 1500s, not 300: the suite ran in ~168s only for as long as its frame-budget
# fixtures were failing to allocate their grids instantly. Once the heap was
# freed and the scenes actually ran, a full pass took 1,125,726 ms.
report_timeout=1500

# A capture where the suites were compiled in but never ran, or where every
# fixture failed to allocate, still parses into a plausible-looking table.
# This line is the proof a frame-budget test got far enough to measure.
report_sentinel="device_tests: sand_step on"

BASELINE=""
remaining=$#
while [ "$remaining" -gt 0 ]; do
    arg="$1"
    shift
    remaining=$((remaining - 1))
    case "$arg" in
        --perf-scope)
            report_build_flags="--perf-scope"
            ;;
        --baseline)
            if [ "$remaining" -eq 0 ]; then
                echo "ERROR: --baseline requires a path" >&2
                exit 1
            fi
            BASELINE="$1"
            shift
            remaining=$((remaining - 1))
            ;;
        --baseline=*)
            BASELINE="${arg#--baseline=}"
            ;;
        *)
            set -- "$@" "$arg"
            ;;
    esac
done

# Both halves of a frame: the simulation's budgets live in the sand suite,
# the draw's live in the gfx one. Reporting only the first hid the fact that
# a present costs as much as a step.
report_generate() {
    python "$SCRIPT_DIR/report_performance.py" "$1" "$2" \
        --source "$LAUNCHER_DIR/main/apps/sand/tests/suite_sand_perf.c" \
        --source "$LAUNCHER_DIR/test/suites/suite_gfx.c"
}

# The measured (not budget) column of one row of report_performance.py's
# table: "| `name` | budget | measured | headroom | status |". Anchored at
# the start of the line so it only matches an actual table row, not `name`
# appearing in one of the report's prose bullet lists.
extract_measured() {
    # `|| true`: awk exits nonzero if the report cannot be opened, and that
    # failure inside a `var=$(...)` assignment would abort the whole script.
    awk -F'|' -v name="$1" '
        $0 ~ "^\\| *`" name "`" { v = $4; gsub(/^[ \t]+|[ \t]+$/, "", v); print v; exit }
    ' "$2" 2>/dev/null || true
}

# Printed here instead of left to the operator - it was already being typed
# by hand five times in two days. Free heap first (a short heap means every
# frame-budget fixture failed to allocate), then the two liquid-free
# controls, whose value-pair tells a real regression from ordinary
# flash-layout noise before reading anything else.
report_summary() {
    echo "=== Summary ==="
    heap_line="$(grep -m1 "free heap after framebuffer" "$1" || true)"
    if [ -z "$heap_line" ]; then
        echo "WARNING: no 'free heap after framebuffer' line found in $1"
    else
        heap="$(printf '%s\n' "$heap_line" | grep -o '[0-9]\+ bytes' | grep -o '[0-9]\+' || true)"
        echo "free heap after framebuffer: ${heap:-?} bytes"
        if [ -n "$heap" ] && [ "$heap" -lt 50000 ]; then
            echo "WARNING: free heap ($heap bytes) is below ~50,000 - frame-budget"
            echo "fixtures likely failed to allocate their grids and measured"
            echo "nothing this run."
        fi
    fi
    for ctrl in test_a_full_size_step_fits_in_the_frame_budget \
                test_flipping_gravity_on_a_settled_pile_fits_in_the_frame_budget; do
        v="$(extract_measured "$ctrl" "$2")"
        echo "control $ctrl: ${v:-not measured this capture} us"
    done

    if [ -n "$BASELINE" ]; then
        echo "=== Comparing against baseline: $BASELINE ==="
        # A non-win verdict, or an error like a missing control row, is a
        # normal outcome to print and read rather than a failure of this run.
        python "$SCRIPT_DIR/compare_reports.py" --verdict "$BASELINE" "$2" || true
    fi
}

# shellcheck source=../../../../tools/device_report.sh
. "$LAUNCHER_DIR/tools/device_report.sh"
device_report_run "$@"

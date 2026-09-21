#!/bin/sh
#
# One-click device self-test report: build+flash the diagnostics image,
# capture the boot-time run of every suite, write a markdown results report.
#
# Usage:
#   tools/report_test_results.sh [--no-restore] [COM_PORT] [OUT.md]
#
#   COM_PORT     serial port the device is on. Found by USB identity when
#                omitted - see scripts/device/device.py.
#   OUT.md       markdown report path. Default:
#                tools/results/test_results_<timestamp>.md
#   --no-restore leave the device on the diagnostics image afterwards.
#
# CAPTURE_TIMEOUT overrides the capture window. 3000s, not the 300 this
# started with: a full pass took 1,125,726 ms once the frame-budget fixtures
# could allocate their grids, and two consecutive captures were then cut off
# at 1500s inside the sand suites, with no SELFTEST_COMPLETE and no `us per
# step` line from the tests that run late. A run that only needs the early
# suites does not have to wait out the whole window.
#
# Everything this does beyond the declarations below - which image, deleting
# a build directory's sdkconfig that disagrees, asserting the flags took,
# capturing, validating, restoring release - is tools/device_report.sh.

set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

report_name=test_results
report_dir="$SCRIPT_DIR/results"
report_timeout="${CAPTURE_TIMEOUT:-3000}"
report_suite=""

# Exit 1 here means the capture contains a failing test, which is what this
# report exists to show. The reporter refuses an empty capture with a
# different code, and that one is fatal.
report_failures_ok=1

report_generate() {
    python "$SCRIPT_DIR/report_test_results.py" "$1" "$2"
}

# shellcheck source=./device_report.sh
. "$SCRIPT_DIR/device_report.sh"
device_report_run "$@"

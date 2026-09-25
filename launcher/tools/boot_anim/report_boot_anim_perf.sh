#!/bin/sh
#
# One-click boot_anim perf report: build+flash the diagnostics image with the
# suites compiled in but NOT running at boot, so the shell - and its RUNSUITE
# listener, see main/util/screenshot.c - comes up in seconds; trigger just
# suite_boot_anim_perf.c via RUNSUITE, capture its output, and write a
# markdown report of the six-checkpoint breakdown.
#
# Declaring a suite is what selects that image: see tools/device/device_report.sh.
#
# Usage:
#   tools/boot_anim/report_boot_anim_perf.sh [--no-restore] [COM_PORT] [OUT.md]
#
#   COM_PORT     serial port the device is on. Found by USB identity when
#                omitted - see scripts/device/device.py.
#   OUT.md       markdown report path. Default:
#                tools/results/boot_anim_perf_<timestamp>.md
#   --no-restore leave the device on the diagnostics image afterwards.
#
# Everything this does beyond the declarations below - which image, deleting
# a build directory's sdkconfig that disagrees, asserting the flags took,
# capturing, validating, restoring release - is tools/device/device_report.sh.

set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

report_name=boot_anim_perf
report_dir="$SCRIPT_DIR/../results"
report_suite=run_boot_anim_perf_suite

# A fixed window after the command is sent: no completion marker is generic
# across suites the way SELFTEST_COMPLETE is for a whole run, so a short
# timeout truncates the tail of the capture rather than erroring.
report_timeout=60

# The header each checkpoint prints before its own breakdown. Without one,
# the RUNSUITE line never reached a listener, or the suite is not in the
# image.
report_sentinel="boot_anim_perf: === BOOT_ANIM PERF"

report_generate() {
    python "$SCRIPT_DIR/report_boot_anim_perf.py" "$1" "$2"
}

# shellcheck source=./device_report.sh
. "$SCRIPT_DIR/../device/device_report.sh"
device_report_run "$@"

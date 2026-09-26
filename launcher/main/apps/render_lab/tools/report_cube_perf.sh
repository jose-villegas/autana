#!/bin/sh
#
# One-click cube perf report: build+flash the diagnostics image, capture the
# boot-time run, write a markdown comparison of suite_cube_perf.c's
# with_hud/no_hud/full_clear/interlaced variants. That suite's output shape
# is different enough from a frame-budget one - a multi-line breakdown per
# run rather than one assertion per test - that it needs its own parser.
#
# Usage:
#   main/apps/render_lab/tools/report_cube_perf.sh [--no-restore] [BOARD] \
#       [OUT.md]
#
#   BOARD        the board's USB serial number. Default: AUTANA_BOARD, else
#                the only board plugged in - see scripts/device/device.py.
#   OUT.md       markdown report path. Default:
#                main/apps/render_lab/tools/results/cube_perf_<timestamp>.md
#   --no-restore leave the device on the diagnostics image afterwards.
#
# Everything this does beyond the declarations below - which image, deleting
# a build directory's sdkconfig that disagrees, asserting the flags took,
# capturing, validating, restoring release - is tools/device/device_report.sh.

set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
# tools -> render_lab -> apps -> main -> launcher.
LAUNCHER_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/../../../.." && pwd)

report_name=cube_perf
# The app's own results dir - deleting main/apps/render_lab/ takes its
# scratch output with it too, same as sand's.
report_dir="$SCRIPT_DIR/results"
report_suite=""

# cube_perf alone runs three 10s captures plus a quick interlaced one - well
# past a suite's normal ~1s, so this needs real headroom.
report_timeout=300

# The header each variant prints before its own breakdown. Without one, the
# capture has nothing for the reporter to table.
report_sentinel="cube_perf: === CUBE PERF"

report_generate() {
    python "$SCRIPT_DIR/report_cube_perf.py" "$1" "$2"
}

# shellcheck source=../../../../tools/device/device_report.sh
. "$LAUNCHER_DIR/tools/device/device_report.sh"
device_report_run "$@"

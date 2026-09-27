#!/usr/bin/env bash
#
# Build the launcher's firmware. Writing it to the board is the other half of
# `autana flash`: device.py runs this script and then, under the board's
# lock, scripts/device/flash_image.sh - the only one of the two that opens
# the serial port. Nothing here needs a board or a lock.
#
# Usage:
#   tools/build/build_flash.sh [--dev|--diag] [--autorun] [--perf-scope]
#                              [--verbose] [IDF_EXPORT]
#
#   --verbose   stream and save the build output. The full stream is in the
#               printed log path in either mode.
#   --dev       build the DEVELOPMENT image (build.dev/) instead of the
#               release one (build/): development-only logging and
#               instrumentation (frame timings, the screenshot listener)
#               plus the Diagnostics app, but no test suites. See below.
#   --diag      build the DIAGNOSTICS image (build.diag/): everything --dev
#               gets you, plus the on-device test suites and Diagnostics' own
#               button for running them. See below.
#   --autorun   with --diag only: layer sdkconfig.defaults.diag_autorun, so
#               the suites run at boot instead of waiting for Diagnostics'
#               button. A serial capture of SELFTEST_COMPLETE needs this;
#               interactive use does not, and pays a full suite run per boot
#               for it. Without the flag AUTORUN must be ABSENT, so a build
#               directory left behind by a capture is regenerated here.
#   --perf-scope  with --diag only: layer sdkconfig.defaults.diag_perf, so the
#               image carries only the perf sources apps declare. Frees the
#               static RAM a capture needs to instrument itself; drops
#               behaviour coverage, so never a merge gate, and its numbers
#               compare only with other perf-scoped captures.
#   IDF_EXPORT  path to ESP-IDF's export script - export.bat on Windows,
#               export.sh elsewhere. Default: the one under $IDF_PATH.
#
# Run from anywhere (it cds to launcher/ itself); double-click from Explorer
# if .sh is associated with Git Bash, or right-click launcher/tools/ ->
# "Git Bash Here" -> `./build_flash.sh`.
#
# All the logic here is POSIX sh. On Windows the ESP-IDF calls go through
# tools/build/idf_shim.bat, which exists only to delete MSYSTEM - see tools/build/idf.sh
# for the ESP-IDF environment setup.
#
# WHY --dev AND --diag BOTH EXIST
#
# CONFIG_LAUNCHER_DEVELOPMENT and CONFIG_LAUNCHER_SELFTEST answer different
# questions - the first is "does this build carry anything meant only for a
# developer at the console" (frame timings, the screenshot listener, and the
# Diagnostics app, whose folder main/CMakeLists.txt excludes unless it is
# set), the second is "does this build carry the on-device test suites",
# which also brings Diagnostics' own button for re-running them. SELFTEST
# `select`s DEVELOPMENT, so a diag build gets both; DEVELOPMENT alone does
# not pull SELFTEST in (see main/Kconfig.projbuild). --dev and --diag are
# what expose that same independence here, rather than only ever being able
# to get instrumentation bundled with the test harness's own footprint -
# every suite linked in, and run at boot before the shell starts - which is
# fine on a bench and unwanted just to watch a frame-timing log line or pull
# a screenshot. Note the Diagnostics app carries its own side effects in
# EITHER build: entering it re-runs POST, cycling the audio rail - that is
# the cost of the way in being compiled at all, not of the test suites.

set -euo pipefail

VARIANT=release
PERF_SCOPE=0
AUTORUN=0
VERBOSE=0
IDF_EXPORT_ARG=""

while [ $# -gt 0 ]; do
    case "$1" in
        --dev)     VARIANT=dev; shift ;;
        -d|--diag) VARIANT=diag; shift ;;
        --autorun) AUTORUN=1; shift ;;
        --perf-scope) PERF_SCOPE=1; shift ;;
        --verbose) VERBOSE=1; shift ;;
        -h|--help) sed -n '2,/^# the cost of the way in/p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        --)        shift; break ;;
        -*)        echo "unknown option: $1" >&2; exit 2 ;;
        *)
            if [ -n "$IDF_EXPORT_ARG" ]; then
                echo "too many positional arguments" >&2
                exit 2
            fi
            IDF_EXPORT_ARG=$1
            shift
            ;;
    esac
done
export VERBOSE

if [ "$PERF_SCOPE" -eq 1 ] && [ "$VARIANT" != diag ]; then
    echo "--perf-scope scopes which SUITES are compiled in, so it needs --diag" >&2
    exit 2
fi

if [ "$AUTORUN" -eq 1 ] && [ "$VARIANT" != diag ]; then
    echo "--autorun runs the SUITES at boot, so it needs --diag" >&2
    exit 2
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LAUNCHER_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"

case "$VARIANT" in
    release) BUILD_DIR="build" ;;
    *)       BUILD_DIR="build.$VARIANT" ;;
esac

# shellcheck source=./idf.sh
. "$SCRIPT_DIR/idf.sh"
# shellcheck source=../../../scripts/quiet.sh
. "$SCRIPT_DIR/../../../scripts/quiet.sh"
idf_init "$LAUNCHER_DIR" "$IDF_EXPORT_ARG" "$SCRIPT_DIR" || exit 2
. "$SCRIPT_DIR/idf_variant.sh"

# A passing build's stream belongs in build.log under the build directory.
QUIET_LOG="$LAUNCHER_DIR/$BUILD_DIR/build.log"
quiet_begin "$QUIET_LOG"

quiet_finish() {
    status=$?
    trap - EXIT
    set +e
    quiet_end build_flash "$status" || true
    if [ "$status" -ne 0 ]; then
        echo
        echo "=== FAILED (exit $status) ==="
        read -r -p "Press Enter to close..." _ || true
    fi
    exit "$status"
}
trap quiet_finish EXIT

VARIANT_OPTIONS=""
if [ "$AUTORUN" -eq 1 ]; then
    VARIANT_OPTIONS="$VARIANT_OPTIONS --autorun"
fi
if [ "$PERF_SCOPE" -eq 1 ]; then
    VARIANT_OPTIONS="$VARIANT_OPTIONS --perf-scope"
fi
# shellcheck disable=SC2086
quiet_run build idf_variant_build "$LAUNCHER_DIR" "$VARIANT" "$BUILD_DIR" $VARIANT_OPTIONS

if [ ! -f "$LAUNCHER_DIR/$BUILD_DIR/build_id.txt" ]; then
    echo "build reported success but produced no build id at" >&2
    echo "  $LAUNCHER_DIR/$BUILD_DIR/build_id.txt" >&2
    exit 1
fi
BUILD_ID=$(tr -d '\r\n' < "$LAUNCHER_DIR/$BUILD_DIR/build_id.txt")
# device.py takes the flashed build's id from this line in the flash log; it
# stays on stdout, outside quiet_run, or no flash can name what it wrote.
echo "BUILD_ID=$BUILD_ID"

echo "=== Done - $BUILD_DIR built ==="

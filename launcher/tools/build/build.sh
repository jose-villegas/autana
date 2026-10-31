#!/usr/bin/env bash
#
# Build the launcher's firmware: what `autana build` runs, the way to build
# (docs/cli/Build-and-Flash.md). Writing it to the board is the other half of
# `autana flash`: device.py runs this script before it takes the board's
# lock, and then, under it, scripts/device/flash_image.sh, the only one of
# the two that opens the serial port. Nothing here needs a board or a lock.
#
# Usage:
#   tools/build/build.sh [--dev|--diag] [--autorun] [--perf-scope [--frame-laps]]
#                        [--layout-seed N] [--hot-tunables] [--configure] [--verbose]
#                        [--prepared-build DIR]
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
#   --frame-laps  with --perf-scope only: layer sdkconfig.defaults.diag_perf_laps,
#               timing the phases inside the frame stages (profile/frame_laps.h).
#               The laps' own cost moves frame times: read only the lap lines.
#   --layout-seed N  pad every source's code and rodata by sizes seed N picks
#               (tools/build/layout_pad.py), to sample the cache layouts a
#               timing could have landed on. 0, the default, is the plain build.
#   --configure  only configure the variant's build directory, compile nothing:
#               writes its sdkconfig and compile_commands.json, and no image.
#               For a tool that reads which sources a variant compiles.
#   --prepared-build DIR  build into DIR, the variant folder build_paths.py
#               already prepared (autana flash does, so it can snapshot it).
#
#   ESP-IDF: the checkout at the commit in launcher/idf-pin.json; IDF_PATH set
#   for this command selects another.
#
# Run from anywhere (it cds to launcher/ itself); double-click from Explorer
# if .sh is associated with Git Bash, or right-click launcher/tools/build/ ->
# "Git Bash Here" -> `./build.sh`. A failure in a double-clicked window waits
# for Enter before it closes (scripts/lib/terminal.sh).
#
# All the logic here is POSIX sh. On Windows the ESP-IDF calls go through
# tools/build/idf_shim.bat, which exists only to delete MSYSTEM, see tools/build/idf.sh
# for the ESP-IDF environment setup.
#
# WHY --dev AND --diag BOTH EXIST
#
# CONFIG_LAUNCHER_DEVELOPMENT and CONFIG_LAUNCHER_SELFTEST answer different
# questions; the first is "does this build carry anything meant only for a
# developer at the console" (frame timings, the screenshot listener, and the
# Diagnostics app, whose folder main/CMakeLists.txt excludes unless it is
# set), the second is "does this build carry the on-device test suites",
# which also brings Diagnostics' own button for re-running them. SELFTEST
# `select`s DEVELOPMENT, so a diag build gets both; DEVELOPMENT alone does
# not pull SELFTEST in (see main/Kconfig.projbuild). --dev and --diag are
# what expose that same independence here, rather than only ever being able
# to get instrumentation bundled with the test harness's own footprint:
# every suite linked in, and run at boot before the shell starts, which is
# fine on a bench and unwanted just to watch a frame-timing log line or pull
# a screenshot. Note the Diagnostics app carries its own side effects in
# EITHER build: entering it re-runs POST, cycling the audio rail; that is
# the cost of the way in being compiled at all, not of the test suites.

set -euo pipefail

VARIANT=release
PERF_SCOPE=0
FRAME_LAPS=0
AUTORUN=0
VERBOSE=0
PREPARED_BUILD=""
LAYOUT_SEED=0
HOT_TUNABLES=0
CONFIGURE=0

while [ $# -gt 0 ]; do
    case "$1" in
        --dev)     VARIANT=dev; shift ;;
        -d|--diag) VARIANT=diag; shift ;;
        --autorun) AUTORUN=1; shift ;;
        --perf-scope) PERF_SCOPE=1; shift ;;
        --frame-laps) FRAME_LAPS=1; shift ;;
        --hot-tunables) HOT_TUNABLES=1; shift ;;
        --layout-seed)
            [ $# -ge 2 ] || { echo "--layout-seed needs a number" >&2; exit 2; }
            LAYOUT_SEED=$2; shift 2 ;;
        --configure) CONFIGURE=1; shift ;;
        --verbose) VERBOSE=1; shift ;;
        -h|--help) sed -n '2,/^# the cost of the way in/p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        --)        shift; break ;;
        --prepared-build) PREPARED_BUILD=$2; shift 2 ;;
        -*)        echo "unknown option: $1" >&2; exit 2 ;;
        *) echo "unexpected argument: $1" >&2; exit 2 ;;
    esac
done
export VERBOSE

if [ "$PERF_SCOPE" -eq 1 ] && [ "$VARIANT" != diag ]; then
    echo "--perf-scope scopes which SUITES are compiled in, so it needs --diag" >&2
    exit 2
fi

if [ "$FRAME_LAPS" -eq 1 ] && [ "$PERF_SCOPE" -ne 1 ]; then
    echo "--frame-laps times the perf scope's frame stages, so it needs --perf-scope" >&2
    exit 2
fi

if [ "$AUTORUN" -eq 1 ] && [ "$VARIANT" != diag ]; then
    echo "--autorun runs the SUITES at boot, so it needs --diag" >&2
    exit 2
fi

if [ "$HOT_TUNABLES" -eq 1 ] && [ "$VARIANT" = release ]; then
    echo "--hot-tunables needs --dev or --diag" >&2
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
# shellcheck source=../../../scripts/lib/terminal.sh
. "$SCRIPT_DIR/../../../scripts/lib/terminal.sh"

# Set by quiet_begin once the build directory exists; a failure before that
# has nothing to summarize.
QUIET_LOG=""
quiet_finish() {
    status=$?
    trap - EXIT
    set +e
    [ -z "$QUIET_LOG" ] || quiet_end build "$status" || true
    if [ "$status" -ne 0 ]; then
        echo
        echo "=== FAILED (exit $status) ==="
        pause_if_own_window
    fi
    exit "$status"
}
trap quiet_finish EXIT

idf_init "$LAUNCHER_DIR" "$SCRIPT_DIR" || exit 2
. "$SCRIPT_DIR/idf_variant.sh"

idf_variant_prepare "$LAUNCHER_DIR" "$BUILD_DIR" "$PREPARED_BUILD" || exit 2

# A passing build's stream belongs in build.log under the build directory.
quiet_begin "$IDF_VARIANT_BUILD/build.log"

VARIANT_OPTIONS=""
if [ "$AUTORUN" -eq 1 ]; then
    VARIANT_OPTIONS="$VARIANT_OPTIONS --autorun"
fi
if [ "$PERF_SCOPE" -eq 1 ]; then
    VARIANT_OPTIONS="$VARIANT_OPTIONS --perf-scope"
fi
if [ "$FRAME_LAPS" -eq 1 ]; then
    VARIANT_OPTIONS="$VARIANT_OPTIONS --frame-laps"
fi
VARIANT_OPTIONS="$VARIANT_OPTIONS --layout-seed $LAYOUT_SEED"
if [ "$HOT_TUNABLES" -eq 1 ]; then
    VARIANT_OPTIONS="$VARIANT_OPTIONS --hot-tunables"
fi
if [ "$CONFIGURE" -eq 1 ]; then
    VARIANT_OPTIONS="$VARIANT_OPTIONS --configure"
fi
# shellcheck disable=SC2086
quiet_run build idf_variant_build "$LAUNCHER_DIR" "$VARIANT" "$IDF_VARIANT_BUILD" $VARIANT_OPTIONS

if [ "$CONFIGURE" -eq 1 ]; then
    echo "=== Done - $BUILD_DIR configured, nothing built: $BUILD_DIR/compile_commands.json ==="
    exit 0
fi
sh "$SCRIPT_DIR/write_build_id.sh" "$IDF_VARIANT_BUILD" "$VARIANT"
echo "=== Done - $BUILD_DIR built, build id $(tr -d '\r\n' < "$IDF_VARIANT_BUILD/build_id.txt") ==="

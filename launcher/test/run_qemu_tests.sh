#!/bin/sh
#
# Build the self-test image for Espressif's QEMU and run it with no board.
#
#   ./launcher/test/run_qemu_tests.sh [--perf-scope] [--icount] [--no-build]
#   ./launcher/test/run_qemu_tests.sh --suite <name> [--suite ...] \
#                                     [--touch <down|up>,<x>,<y> ...] [--screenshot <png>]
#   ./launcher/test/run_qemu_tests.sh --perf-scope --suite <name> --build-only
#
# The image is the diagnostics build with sdkconfig.defaults.qemu layered
# last, in its own build.qemu/ (build.qemu.perf/ for --perf-scope), so it
# never reconfigures build/ or build.diag/. qemu_run.py beside this file
# boots it and reads the console; its header says what a run under emulation
# can and cannot tell you, and what --icount counts.
#
# The first form runs every suite at boot. The second builds the same image
# without autorun (build.qemu.shell/), which boots into the shell, and asks
# its console for one suite at a time - seconds each - for a touch to be put
# on the screen, and for the screen itself.
#
# Needs qemu-xtensa, which ESP-IDF does not install by default:
#   python %IDF_PATH%\tools\idf_tools.py install qemu-xtensa
#
# Environment: IDF_EXPORT, as in tools/build_flash.sh.
#
# POSIX sh, same portability reasoning as run_tests.sh.

set -eu

TEST_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
LAUNCHER_DIR=$(CDPATH= cd -- "$TEST_DIR/.." && pwd)

BUILD_DIR=build.qemu
DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.diag"
AUTORUN=1
PERF=0
BUILD=1
RUN=1
RUN_ARGS=""
while [ $# -gt 0 ]; do
    case "$1" in
        --perf-scope) PERF=1 ;;
        --icount) RUN_ARGS="$RUN_ARGS --icount" ;;
        --no-build) BUILD=0 ;;
        # One image serves however many instances a driver then starts
        # against it by hand, each with its own qemu_run.py --workdir.
        --build-only) RUN=0 ;;
        --suite | --touch | --do | --screenshot)
            [ $# -ge 2 ] || { echo "$1 needs a value" >&2; exit 2; }
            AUTORUN=0
            RUN_ARGS="$RUN_ARGS $1 $2"
            shift
            ;;
        *)
            echo "usage: run_qemu_tests.sh [--perf-scope] [--icount] [--no-build] [--build-only]" >&2
            echo "       run_qemu_tests.sh --suite <name> [--suite ...] [--screenshot <png>]" >&2
            exit 2
            ;;
    esac
    shift
done
if [ "$AUTORUN" = 1 ]; then
    DEFAULTS="$DEFAULTS;sdkconfig.defaults.diag_autorun"
else
    BUILD_DIR=build.qemu.shell
fi
if [ "$PERF" = 1 ]; then
    BUILD_DIR="$BUILD_DIR.perf"
    DEFAULTS="$DEFAULTS;sdkconfig.defaults.diag_perf"
fi
DEFAULTS="$DEFAULTS;sdkconfig.defaults.qemu"

case "$(uname -s)" in
    MINGW* | MSYS* | CYGWIN*) DEFAULT_EXPORT='C:\Espressif\esp-idf-v5.5\export.bat' ;;
    *) DEFAULT_EXPORT="${IDF_PATH:-$HOME/esp/esp-idf}/export.sh" ;;
esac
IDF_EXPORT="${IDF_EXPORT:-$DEFAULT_EXPORT}"

if [ "$BUILD" = 1 ]; then
    # shellcheck source=../tools/idf.sh
    . "$LAUNCHER_DIR/tools/idf.sh"
    idf_init "$LAUNCHER_DIR" "$IDF_EXPORT" "$LAUNCHER_DIR/tools"
    idf -B "$BUILD_DIR" \
        -D SDKCONFIG_DEFAULTS="$DEFAULTS" \
        -D SDKCONFIG="$BUILD_DIR/sdkconfig" \
        build
fi

if [ ! -f "$LAUNCHER_DIR/$BUILD_DIR/launcher.bin" ]; then
    echo "no image at $LAUNCHER_DIR/$BUILD_DIR/launcher.bin" >&2
    exit 1
fi

if [ "$RUN" = 0 ]; then
    echo "$LAUNCHER_DIR/$BUILD_DIR"
    exit 0
fi

PYTHON=$(command -v python3 || command -v python || true)
if [ -z "${PYTHON:-}" ]; then
    echo "no Python found to run qemu_run.py" >&2
    exit 1
fi
# shellcheck disable=SC2086
"$PYTHON" "$TEST_DIR/qemu_run.py" "$LAUNCHER_DIR/$BUILD_DIR" $RUN_ARGS

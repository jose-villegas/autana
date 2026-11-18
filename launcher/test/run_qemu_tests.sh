#!/bin/sh
#
# Build the self-test image for Espressif's QEMU and run it with no board.
#
#   ./launcher/test/run_qemu_tests.sh [--perf-scope] [--icount] [--timeout S] [--no-build]
#   ./launcher/test/run_qemu_tests.sh --icount-ratchet [--no-build]
#   ./launcher/test/run_qemu_tests.sh --suite <name> [--suite ...] [--test <pattern>[,...]] \
#                                 [--touch <down|up>,<x>,<y> ...] [--screenshot <png>]
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
# its console for one suite at a time (seconds each), for a touch to be put
# on the screen, and for the screen itself.
#
# Needs qemu-xtensa, which ESP-IDF does not install by default:
#   python %IDF_PATH%\tools\idf_tools.py install qemu-xtensa
#
# IDF_PATH set for this command selects another checkout.
#
# POSIX sh, same portability reasoning as run_tests.sh.

set -eu

TEST_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
LAUNCHER_DIR=$(CDPATH= cd -- "$TEST_DIR/.." && pwd)

BUILD_DIR=build.qemu
AUTORUN=1
PERF=0
BUILD=1
RUN=1
# What qemu_run.py takes is moved to the end of "$@", each value one word
# however many spaces it holds ('--do "tap 120 200"'), and passed on as is.
left=$#
while [ "$left" -gt 0 ]; do
    case "$1" in
        --perf-scope) PERF=1 ;;
        --icount-ratchet) AUTORUN=0; set -- "$@" "$1" ;;
        --icount) set -- "$@" "$1" ;;
        --no-build) BUILD=0 ;;
        # One image serves however many instances a driver then starts
        # against it by hand, each with its own qemu_run.py --workdir.
        --build-only) RUN=0 ;;
        --timeout | --log | --workdir | --test | --suite | --touch | --do | --screenshot)
            [ "$left" -ge 2 ] || { echo "$1 needs a value" >&2; exit 2; }
            case "$1" in --suite | --touch | --do | --screenshot) AUTORUN=0 ;; esac
            set -- "$@" "$1" "$2"
            shift
            left=$((left - 1))
            ;;
        *)
            echo "usage: run_qemu_tests.sh [--perf-scope] [--icount | --icount-ratchet] [--timeout S] [--no-build] [--build-only]" >&2
            echo "       run_qemu_tests.sh --suite <name> [--suite ...] [--test <pattern>[,<pattern>...]] [--screenshot <png>]" >&2
            exit 2
            ;;
    esac
    shift
    left=$((left - 1))
done
if [ "$AUTORUN" != 1 ]; then
    BUILD_DIR=build.qemu.shell
fi
if [ "$PERF" = 1 ]; then
    BUILD_DIR="$BUILD_DIR.perf"
fi

if [ "$BUILD" = 1 ]; then
    # shellcheck source=../tools/build/idf.sh
    . "$LAUNCHER_DIR/tools/build/idf.sh"
    idf_init "$LAUNCHER_DIR" "$LAUNCHER_DIR/tools/build" || exit 2
    . "$LAUNCHER_DIR/tools/build/idf_variant.sh"
    VARIANT_OPTIONS=""
    if [ "$AUTORUN" = 1 ]; then
        VARIANT_OPTIONS="$VARIANT_OPTIONS --autorun"
    fi
    if [ "$PERF" = 1 ]; then
        VARIANT_OPTIONS="$VARIANT_OPTIONS --perf-scope"
    fi
    VARIANT_OPTIONS="$VARIANT_OPTIONS --qemu"
    idf_variant_prepare "$LAUNCHER_DIR" "$BUILD_DIR" || exit 2
    # shellcheck disable=SC2086
    idf_variant_build "$LAUNCHER_DIR" diag "$IDF_VARIANT_BUILD" $VARIANT_OPTIONS
fi

if [ ! -f "$LAUNCHER_DIR/$BUILD_DIR/launcher.bin" ]; then
    echo "no image at $LAUNCHER_DIR/$BUILD_DIR/launcher.bin" >&2
    exit 1
fi

if [ "$RUN" = 0 ]; then
    echo "$LAUNCHER_DIR/$BUILD_DIR"
    exit 0
fi

# shellcheck source=../../scripts/lib/python.sh
. "$LAUNCHER_DIR/../scripts/lib/python.sh"
PYTHON=$(find_python) || exit 1
"$PYTHON" "$TEST_DIR/qemu_run.py" "$LAUNCHER_DIR/$BUILD_DIR" "$@"

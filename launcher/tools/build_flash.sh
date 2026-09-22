#!/usr/bin/env bash
#
# Build the launcher's firmware and flash it to the device.
#
# Usage:
#   tools/build_flash.sh [--dev|--diag] [--autorun] [--perf-scope] \
#                        [--build-only] [COM_PORT] [IDF_EXPORT]
#
#   --dev       build the DEVELOPMENT image instead of the release one, and
#               leave it on the board: development-only logging and
#               instrumentation (frame timings, the screenshot listener)
#               plus the Diagnostics app, but no test suites. See below.
#   --diag      build the DIAGNOSTICS image instead of the release one, and
#               leave it on the board: everything --dev gets you, plus the
#               on-device test suites and Diagnostics' own button for
#               running them. See below.
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
#   --build-only  build and stop: no device needed, nothing flashed.
#   COM_PORT    the port to flash. Flashing itself needs AUTANA_DEVICE_LOCK_TOKEN
#               in the environment - device.py's own `flash`/`selftest`/`batch`
#               set it after taking the device lock, and pass this worktree's
#               port; run `autana flash rel|dev|diag` rather than this script
#               directly. --build-only needs neither the token nor a port.
#   IDF_EXPORT  path to ESP-IDF's export script - export.bat on Windows,
#               export.sh elsewhere. Default: the ESP-IDF Windows
#               installer's path.
#
# Run from anywhere (it cds to launcher/ itself); double-click from Explorer
# if .sh is associated with Git Bash, or right-click launcher/tools/ ->
# "Git Bash Here" -> `./build_flash.sh`.
#
# All the logic here is POSIX sh. On Windows the ESP-IDF calls go through
# tools/idf_shim.bat, which exists only to delete MSYSTEM - see tools/idf.sh
# for the full story and the measurements behind it. This script used to
# embed a block of PowerShell that carried its own sequencing and exit-code
# handling; it does not need to.
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
#
# Using either flag means putting that image on the board and leaving it.
# Nothing else here does that: `autana selftest` and the report scripts
# flash the diagnostics variant too, but to read test output back, and the
# report scripts restore release afterwards - see tools/device_report.sh.
# Neither is "put this image on the device and leave it there", which is
# what these flags are for.

set -euo pipefail

VARIANT=release
BUILD_ONLY=0
PERF_SCOPE=0
AUTORUN=0

while [ $# -gt 0 ]; do
    case "$1" in
        --dev)     VARIANT=dev; shift ;;
        -d|--diag) VARIANT=diag; shift ;;
        --autorun) AUTORUN=1; shift ;;
        --perf-scope) PERF_SCOPE=1; shift ;;
        --build-only) BUILD_ONLY=1; shift ;;
        -h|--help) sed -n '2,74p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        --)        shift; break ;;
        -*)        echo "unknown option: $1" >&2; exit 2 ;;
        *)         break ;;
    esac
done

if [ "$PERF_SCOPE" -eq 1 ] && [ "$VARIANT" != diag ]; then
    echo "--perf-scope scopes which SUITES are compiled in, so it needs --diag" >&2
    exit 2
fi

if [ "$AUTORUN" -eq 1 ] && [ "$VARIANT" != diag ]; then
    echo "--autorun runs the SUITES at boot, so it needs --diag" >&2
    exit 2
fi

COM_PORT="${1:-}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LAUNCHER_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

if [ -n "${MSYSTEM:-}" ]; then
    DEFAULT_EXPORT='C:\Espressif\esp-idf-v5.5\export.bat'
else
    DEFAULT_EXPORT="${IDF_PATH:-$HOME/esp/esp-idf}/export.sh"
fi
IDF_EXPORT="${2:-$DEFAULT_EXPORT}"

case "$VARIANT" in
    release) BUILD_DIR="build" ;;
    *)       BUILD_DIR="build.$VARIANT" ;;
esac

# So a double-clicked window (which closes the instant the script exits)
# still shows the reason for a failure instead of vanishing on the spot.
trap 'status=$?; if [ $status -ne 0 ]; then echo; echo "=== FAILED (exit $status) ==="; read -r -p "Press Enter to close..." _ || true; fi' EXIT

# shellcheck source=./idf.sh
. "$SCRIPT_DIR/idf.sh"
idf_init "$LAUNCHER_DIR" "$IDF_EXPORT" "$SCRIPT_DIR"

SDKCONFIG_FRAGMENTS=""
REQUIRED_FLAGS=""
FORBIDDEN_FLAGS=""
BUILD_SDKCONFIG="$LAUNCHER_DIR/$BUILD_DIR/sdkconfig"

# The first way the config on disk contradicts what was asked for, or nothing.
# Both halves are needed: a flag that must be there catches a fragment that
# never applied, and a flag that must be ABSENT catches one left behind by a
# different request into the same build directory - the variant directories
# are shared, so the previous run's scope outlives it.
sdkconfig_disagreement() {
    local config="$1" flag
    for flag in $REQUIRED_FLAGS; do
        if ! grep -q "^${flag}=y" "$config"; then
            echo "$flag is not set"
            return 0
        fi
    done
    for flag in $FORBIDDEN_FLAGS; do
        if grep -q "^${flag}=y" "$config"; then
            echo "$flag is set"
            return 0
        fi
    done
    return 1
}

if [ "$VARIANT" != release ]; then
    SDKCONFIG_FRAGMENTS="sdkconfig.defaults;sdkconfig.defaults.$VARIANT"
    REQUIRED_FLAGS="CONFIG_LAUNCHER_DEVELOPMENT"
    if [ "$VARIANT" = diag ]; then
        REQUIRED_FLAGS="$REQUIRED_FLAGS CONFIG_LAUNCHER_SELFTEST"
        # Autorun is the only difference between the interactive diag image
        # and the one a capture needs, and both land in the same build.diag.
        # Whichever ran last, the request decides which one is on disk here.
        if [ "$AUTORUN" -eq 1 ]; then
            SDKCONFIG_FRAGMENTS="$SDKCONFIG_FRAGMENTS;sdkconfig.defaults.diag_autorun"
            REQUIRED_FLAGS="$REQUIRED_FLAGS CONFIG_LAUNCHER_SELFTEST_AUTORUN"
        else
            FORBIDDEN_FLAGS="CONFIG_LAUNCHER_SELFTEST_AUTORUN"
        fi
    else
        FORBIDDEN_FLAGS="CONFIG_LAUNCHER_SELFTEST"
    fi
    if [ "$PERF_SCOPE" -eq 1 ]; then
        SDKCONFIG_FRAGMENTS="$SDKCONFIG_FRAGMENTS;sdkconfig.defaults.diag_perf"
        REQUIRED_FLAGS="$REQUIRED_FLAGS CONFIG_LAUNCHER_SELFTEST_SCOPE_PERF"
        echo "=== PERF SCOPE: behaviour suites are NOT in this image ==="
    elif [ "$VARIANT" = diag ]; then
        FORBIDDEN_FLAGS="$FORBIDDEN_FLAGS CONFIG_LAUNCHER_SELFTEST_SCOPE_PERF"
    fi

    # A generated sdkconfig WINS over the fragments: idf.py applies
    # SDKCONFIG_DEFAULTS only when it has to CREATE that file, so an existing
    # one silently outranks every fragment edit and every fragment this run did
    # or did not layer. Deleting it whenever it is older than a fragment or
    # disagrees with the request costs one reconfigure and makes the request
    # authoritative again. Every request for a device image comes through here,
    # so switching between two of them into one build directory pays that
    # reconfigure and a wrong image does not reach the board.
    if [ -f "$BUILD_SDKCONFIG" ]; then
        stale=""
        for fragment in $(printf '%s' "$SDKCONFIG_FRAGMENTS" | tr ';' ' '); do
            if [ "$LAUNCHER_DIR/$fragment" -nt "$BUILD_SDKCONFIG" ]; then
                stale="$fragment is newer"
                break
            fi
        done
        if [ -z "$stale" ]; then
            stale="$(sdkconfig_disagreement "$BUILD_SDKCONFIG")" || stale=""
        fi
        if [ -n "$stale" ]; then
            echo "=== $BUILD_DIR/sdkconfig: $stale - regenerating it ==="
            rm -f "$BUILD_SDKCONFIG"
        fi
    fi
fi

echo "=== Building $BUILD_DIR ==="
if [ -n "$REQUIRED_FLAGS" ]; then
    # Said out loud, not only on failure: which flags a request demands is
    # the thing that drifted silently twice, and a capture's log is where
    # anyone looks afterwards to find out what was actually built.
    # Unquoted on purpose: one space between flags however they were built up.
    # shellcheck disable=SC2086
    echo "=== $BUILD_DIR/sdkconfig must have:" $REQUIRED_FLAGS "==="
    if [ -n "${FORBIDDEN_FLAGS# }" ]; then
        # shellcheck disable=SC2086
        echo "===   and must not have:" $FORBIDDEN_FLAGS "==="
    fi
fi
if [ "$VARIANT" = release ]; then
    idf -B "$BUILD_DIR" build
else
    # BOTH -D flags are needed, and the second is the one that is easy to
    # leave off and hard to notice missing. idf.py's default sdkconfig path
    # is the PROJECT's sdkconfig, not the build directory's, so without
    # -D SDKCONFIG this reads launcher/sdkconfig - which has neither flag on
    # - and cheerfully produces a build.dev or build.diag image with none of
    # it in there. It can even look like it worked, if that build directory's
    # own CMakeCache.txt is left over from a run that did pass the flag.
    # SDKCONFIG_DEFAULTS alone does not fix it. (Learned in the report path,
    # where the silent version of this cost a capture with no measurements
    # in it.)
    #
    # The semicolon inside SDKCONFIG_DEFAULTS reaches idf.py intact on
    # Windows too: passing arguments through the shim without mangling them
    # is exactly what idf_shim.bat is for, and what ci_check_idf_sh.sh
    # asserts for the POSIX branch.
    idf -B "$BUILD_DIR" \
        -D SDKCONFIG_DEFAULTS="$SDKCONFIG_FRAGMENTS" \
        -D SDKCONFIG="$BUILD_DIR/sdkconfig" \
        build

    # The generated config is the only honest witness that the fragments took.
    # The delete above is not enough on its own: a renamed Kconfig symbol or a
    # dropped fragment leaves a config that exists, builds, flashes, and is
    # simply not the image that was asked for.
    disagreement="$(sdkconfig_disagreement "$BUILD_SDKCONFIG")" || disagreement=""
    if [ -n "$disagreement" ]; then
        echo "$BUILD_DIR/sdkconfig: $disagreement - the fragments did not reach" >&2
        echo "this build, so the image is not the one asked for." >&2
        exit 1
    fi
fi

# A build tool that reports success without producing anything is how this
# project once flashed and measured code it had never built. The exit
# status is not enough on its own, so confirm the artifact is really there.
if [ ! -f "$LAUNCHER_DIR/$BUILD_DIR/launcher.bin" ]; then
    echo "build reported success but produced no binary at" >&2
    echo "  $LAUNCHER_DIR/$BUILD_DIR/launcher.bin" >&2
    exit 1
fi

if [ ! -f "$LAUNCHER_DIR/$BUILD_DIR/build_id.txt" ]; then
    echo "build reported success but produced no build id at" >&2
    echo "  $LAUNCHER_DIR/$BUILD_DIR/build_id.txt" >&2
    exit 1
fi
BUILD_ID=$(tr -d '\r\n' < "$LAUNCHER_DIR/$BUILD_DIR/build_id.txt")
echo "BUILD_ID=$BUILD_ID"

if [ "$BUILD_ONLY" -eq 1 ]; then
    # Reaching this line IS the result: nothing is flashed and no device
    # has to be attached.
    echo "=== Done - $BUILD_DIR built, nothing flashed ==="
    exit 0
fi

# Flashing touches the one shared board, so it needs the device lock - this
# refuses without proof one is held, rather than opening the port itself
# and risking two writers. device.py sets AUTANA_DEVICE_LOCK_TOKEN (and
# passes COM_PORT) once it holds the lock; nothing else should set it.
if [ -z "${AUTANA_DEVICE_LOCK_TOKEN:-}" ]; then
    echo "ERROR: flashing needs the device lock." >&2
    echo "Run 'autana flash rel|dev|diag' instead of this script directly," >&2
    echo "or pass --build-only, which needs neither the lock nor a device." >&2
    exit 1
fi
if [ -z "$COM_PORT" ]; then
    echo "ERROR: no COM_PORT given - device.py always passes one under the lock." >&2
    exit 1
fi

echo "=== Flashing to $COM_PORT ==="
idf -B "$BUILD_DIR" -p "$COM_PORT" flash

case "$VARIANT" in
    dev)
        echo "=== Done - the DEVELOPMENT image is on the device ==="
        echo "    Development-only logging and instrumentation is on, and the"
        echo "    Diagnostics app is there (BOOT for its toggles page); no test"
        echo "    suites. Re-run without --dev to put release back."
        ;;
    diag)
        echo "=== Done - the DIAGNOSTICS image is on the device ==="
        if [ "$AUTORUN" -eq 1 ]; then
            echo "    AUTORUN is layered: every compiled-in suite runs at boot,"
            echo "    before the shell comes up, and the run ends in a"
            echo "    SELFTEST_COMPLETE line on the console."
        else
            echo "    The test suites are compiled in - Diagnostics' own button runs"
            echo "    them on demand, since AUTORUN is not layered here."
        fi
        if [ "$PERF_SCOPE" -eq 1 ]; then
            echo "    PERF-SCOPED: only sand's frame-budget suite and its scenes are"
            echo "    in there. Its numbers compare only with other perf-scoped"
            echo "    captures, and it is not a behaviour gate."
        fi
        echo "    Re-run without --diag to put the release firmware back."
        ;;
    *)
        echo "=== Done ==="
        ;;
esac

# `|| true` because this is the LAST command: with stdin at EOF (piped, or
# redirected from /dev/null in CI) read returns non-zero, which became the
# script's exit status and made the trap above announce a failure over a
# perfectly good flash. The pause is a convenience for double-clickers, not
# a step that can fail.
read -r -p "Press Enter to close..." _ || true

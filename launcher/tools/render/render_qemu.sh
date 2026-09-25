#!/bin/sh
#
# Capture the screen from the device image running under QEMU, and compare
# it with the host render of the same screen.
#
#   ./launcher/tools/render/render_qemu.sh [-o <dir>] [--no-build]
#   ./launcher/tools/render/render_qemu.sh [-o <dir>] --row <label> [--row <label>]...
#
# The second backend of the host render harness: the same scenes, the real
# Xtensa image instead of a host build. The capture itself is entirely
# test/run_qemu_tests.sh's - it builds the no-autorun image in
# build.qemu.shell/, boots it, asks its console for the screen and writes a
# PNG plus a state .json exactly as autana screenshot does from a board.
# Nothing here re-implements any of that.
#
# With one or more --row, the home screen is also rendered on the host with
# those rows and diffed against the capture, pixel for pixel, with the
# shell's own chrome masked by declaration (see render_masks.json). The rows
# have to be stated because only the image knows what registered itself: its
# console reports how many, never which.
#
# The quarter comes from the capture's own sidecar, never from the image's
# shape. Under emulation there is no IMU, so the shell keeps the orientation
# it starts at.
#
# About a minute and a half after the build. POSIX sh, like the rest of this
# directory.

set -eu

TOOLS_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
LAUNCHER_DIR=$(CDPATH= cd -- "$TOOLS_DIR/../.." && pwd)

OUT_DIR="$TOOLS_DIR/../results/render/qemu"
BUILD_ARGS=""
ROWS=""
while [ $# -gt 0 ]; do
    case "$1" in
        -o) OUT_DIR="$2"; shift 2 ;;
        --no-build) BUILD_ARGS="$BUILD_ARGS --no-build"; shift ;;
        --row)
            [ $# -ge 2 ] || { echo "--row needs a label" >&2; exit 2; }
            ROWS="$ROWS --row '$2'"
            shift 2
            ;;
        *)
            echo "usage: $0 [-o <dir>] [--no-build] [--row <label>]..." >&2
            exit 2
            ;;
    esac
done

if ! PYTHON=$(command -v python3 || command -v python); then
    echo "No Python found; the capture and the diff both need one." >&2
    exit 1
fi

mkdir -p "$OUT_DIR"
CAPTURE="$OUT_DIR/qemu.png"

# shellcheck disable=SC2086
sh "$LAUNCHER_DIR/test/run_qemu_tests.sh" $BUILD_ARGS --screenshot "$CAPTURE"

if [ ! -f "$CAPTURE" ]; then
    echo "the run finished but wrote no capture at $CAPTURE" >&2
    exit 1
fi
echo "captured $CAPTURE"

if [ -z "$ROWS" ]; then
    echo "state the image's rows with --row to compare it against a host render"
    exit 0
fi

QUARTER=$("$PYTHON" -c "import json,sys; print(json.load(open(sys.argv[1]))['orientation_quarter'])" \
    "${CAPTURE%.png}.json")
echo "the capture's sidecar says quarter $QUARTER"

sh "$TOOLS_DIR/scenes/launcher_home_render_host.sh" -o "$OUT_DIR/host" > /dev/null
BIN="$OUT_DIR/host/launcher_home_render"
if [ "${OS:-}" = "Windows_NT" ]; then
    BIN="$BIN.exe"
fi

# Two frames and no finger: a settled screen, which is what an idle device
# was showing when the capture was taken.
eval "\"\$BIN\" --quarter \$QUARTER --panel --frames 2 $ROWS -o \"\$OUT_DIR/host_home.bmp\"" 2> /dev/null

exec sh "$TOOLS_DIR/render_diff.sh" "$CAPTURE" "$OUT_DIR/host_home.bmp" \
    --mask build_mark --mask home_hint --out "$OUT_DIR/diff.png"

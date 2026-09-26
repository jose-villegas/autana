#!/usr/bin/env bash
#
# ems.2 acceptance 3: builds the diagnostics image with one GFX_BAND_HEIGHT
# choice (16, 32 or 64 - see main/Kconfig.projbuild), so a device sweep
# across heights is the same command three times rather than a hand-edited
# sdkconfig each time. Never flashes or opens a serial port - band-per-band
# perf is a device measurement someone at the board has to take.
#
# Usage:
#   tools/sweeps/band_height_sweep.sh 16|32|64 [IDF_EXPORT]
#
# Leaves build.diag.bh<N>/launcher.bin built, and flashes nothing: every
# flash goes through the board's lock (`autana flash`, see
# docs/tools/Device-Lock.md), and `autana flash` builds and flashes only its
# own variants, which this build directory is not. With a band-height image
# on the board, capture under the same lock:
#   python scripts/device/device.py --owner <you> run-suite run_cube_band_perf_suite \
#       --out out.txt --purpose "band height $N"
# out.txt's "CUBE BAND VS FULL-FB" line has present/rasterize timing for
# both arms; boot's own HEAPMARK lines (main.c) have the largest free block.
set -euo pipefail

if [ $# -lt 1 ] || { [ "$1" != 16 ] && [ "$1" != 32 ] && [ "$1" != 64 ]; }; then
    echo "usage: $0 16|32|64 [IDF_EXPORT]" >&2
    exit 2
fi
HEIGHT="$1"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LAUNCHER_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"

# shellcheck source=../idf.sh
. "$SCRIPT_DIR/../build/idf.sh"
idf_init "$LAUNCHER_DIR" "${2:-}" "$SCRIPT_DIR/../build" || exit 2
. "$SCRIPT_DIR/../build/idf_variant.sh"

BUILD_DIR="build.diag.bh$HEIGHT"
DEFAULTS_FILE="$LAUNCHER_DIR/sdkconfig.defaults.band_height_$HEIGHT"

# A fourth defaults layer, same idea as sdkconfig.defaults.diag_autorun
# layering onto sdkconfig.defaults.diag - one line, applied on top of the
# diag build's own defaults, never touching main/Kconfig.projbuild's
# default (32) for a plain build.
printf 'CONFIG_LAUNCHER_GFX_BAND_HEIGHT_%s=y\n' "$HEIGHT" > "$DEFAULTS_FILE"

echo "=== Building $BUILD_DIR (GFX_BAND_HEIGHT=$HEIGHT) ==="
idf_variant_build "$LAUNCHER_DIR" diag "$BUILD_DIR" \
    --defaults "sdkconfig.defaults.band_height_$HEIGHT" "CONFIG_LAUNCHER_GFX_BAND_HEIGHT_$HEIGHT"

rm -f "$DEFAULTS_FILE"

if [ ! -f "$LAUNCHER_DIR/$BUILD_DIR/launcher.bin" ]; then
    echo "build reported success but produced no binary at $LAUNCHER_DIR/$BUILD_DIR/launcher.bin" >&2
    exit 1
fi

echo "=== Done - $BUILD_DIR built, nothing flashed ==="
echo "Flashing:     not by this script - see its header"
echo "Capture with: python scripts/device/device.py --owner <you> run-suite run_cube_band_perf_suite --out $BUILD_DIR.out.txt"

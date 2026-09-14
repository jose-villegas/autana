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
# Leaves build.diag.bh<N>/launcher.bin built. Flash and capture it with the
# tools this tree already has:
#   idf.py -B build.diag.bh<N> -p COM3 flash
#   tools/sweeps/capture_runsuite.py run_cube_band_perf_suite out.txt --port COM3
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

if [ -n "${MSYSTEM:-}" ]; then
    DEFAULT_EXPORT='C:\Espressif\esp-idf-v5.5\export.bat'
else
    DEFAULT_EXPORT="${IDF_PATH:-$HOME/esp/esp-idf}/export.sh"
fi
IDF_EXPORT="${2:-$DEFAULT_EXPORT}"

# shellcheck source=../idf.sh
. "$SCRIPT_DIR/../idf.sh" idf_init "$LAUNCHER_DIR" "$IDF_EXPORT" "$SCRIPT_DIR/.."

BUILD_DIR="build.diag.bh$HEIGHT"
DEFAULTS_FILE="$LAUNCHER_DIR/sdkconfig.defaults.band_height_$HEIGHT"

# A fourth defaults layer, same idea as sdkconfig.defaults.diag_autorun
# layering onto sdkconfig.defaults.diag - one line, applied on top of the
# diag build's own defaults, never touching main/Kconfig.projbuild's
# default (32) for a plain build.
printf 'CONFIG_LAUNCHER_GFX_BAND_HEIGHT_%s=y\n' "$HEIGHT" > "$DEFAULTS_FILE"

echo "=== Building $BUILD_DIR (GFX_BAND_HEIGHT=$HEIGHT) ==="
idf -B "$BUILD_DIR" \
    -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.diag;sdkconfig.defaults.band_height_$HEIGHT" \
    -D SDKCONFIG="$BUILD_DIR/sdkconfig" \
    build

rm -f "$DEFAULTS_FILE"

if [ ! -f "$LAUNCHER_DIR/$BUILD_DIR/launcher.bin" ]; then
    echo "build reported success but produced no binary at $LAUNCHER_DIR/$BUILD_DIR/launcher.bin" >&2
    exit 1
fi

echo "=== Done - $BUILD_DIR built, nothing flashed ==="
echo "Flash with:   idf.py -B $BUILD_DIR -p COM3 flash"
echo "Capture with: tools/sweeps/capture_runsuite.py run_cube_band_perf_suite $BUILD_DIR.out.txt --port COM3"

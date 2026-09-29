#!/bin/sh
#
# Build and run colour_lever1_counters.c: cells marked and bytes sent per
# frame, before and after lever 1's own suppression, on a busy landscape
# scene - see that file's own top comment.
#
# Usage:
#   main/apps/sand/tools/report_colour_lever1_counters.sh

set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
SAND_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
MAIN_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/../../.." && pwd)
LAUNCHER_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/../../../.." && pwd)

BUILD_DIR="$SCRIPT_DIR/build"

# shellcheck source=../../../../tools/build/find_cc.sh
. "$LAUNCHER_DIR/tools/build/find_cc.sh"

if ! CC_BIN=$(find_cc); then
    echo "No C compiler found." >&2
    echo "  Windows: winget install BrechtSanders.WinLibs.POSIX.UCRT" >&2
    echo "  Debian:  sudo apt install build-essential" >&2
    echo "  macOS:   xcode-select --install" >&2
    exit 1
fi

CFLAGS="-std=c11 -Wall -Wextra -Werror -Wno-unused-parameter"

mkdir -p "$BUILD_DIR"
OUT_BIN="$BUILD_DIR/colour_lever1_counters"

# shellcheck disable=SC2086
"$CC_BIN" $CFLAGS -I "$MAIN_DIR" -I "$SAND_DIR" \
    "$SCRIPT_DIR/colour_lever1_counters.c" \
    "$MAIN_DIR/util/job.c" \
    "$SAND_DIR/sand.c" \
    "$SAND_DIR/sand_chunk_sched.c" \
    "$SAND_DIR/sand_impulse.c" \
    "$SAND_DIR/sand_reactions.c" \
    "$SAND_DIR/sand_plants.c" \
    "$SAND_DIR/sand_gas.c" \
    "$SAND_DIR/sand_liquid.c" \
    "$SAND_DIR/material.c" \
    "$SAND_DIR/material_palette.c" \
    -o "$OUT_BIN"

[ -x "$OUT_BIN" ] || OUT_BIN="$OUT_BIN.exe"

"$OUT_BIN"

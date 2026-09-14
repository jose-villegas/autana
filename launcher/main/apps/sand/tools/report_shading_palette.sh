#!/bin/sh
#
# Build and run shading_palette.c: every colour sand's shading can produce,
# a proposed 256-entry palette for it, and six landscape scenes rendered
# both ways for a human to judge - see shading_palette.c's own top comment.
#
# Usage:
#   main/apps/sand/tools/report_shading_palette.sh [results-dir]
#
# Writes stats.txt, mapping.csv, palette_swatches.png and one
# scene_<name>.png per scene into results-dir (default: this tool's build/
# folder). The sweep calls material_colours() a few billion times, so a run
# takes a minute or two.

set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
SAND_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
MAIN_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/../../.." && pwd)
LAUNCHER_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/../../../.." && pwd)

BUILD_DIR="$SCRIPT_DIR/build"
RESULTS_DIR="${1:-$BUILD_DIR/shading_palette}"

# shellcheck source=../../../../tools/find_cc.sh
. "$LAUNCHER_DIR/tools/find_cc.sh"

if ! CC_BIN=$(find_cc); then
    echo "No C compiler found." >&2
    echo "  Windows: winget install BrechtSanders.WinLibs.POSIX.UCRT" >&2
    echo "  Debian:  sudo apt install build-essential" >&2
    echo "  macOS:   xcode-select --install" >&2
    exit 1
fi

# run_tests.sh's warnings, at -O2: the sweep is the whole run time.
CFLAGS="-std=c11 -Wall -Wextra -Werror -Wno-unused-parameter -g -O2"

mkdir -p "$BUILD_DIR" "$RESULTS_DIR"
OUT_BIN="$BUILD_DIR/shading_palette"

# shellcheck disable=SC2086
"$CC_BIN" $CFLAGS -I "$MAIN_DIR" -I "$SAND_DIR" \
    "$SCRIPT_DIR/shading_palette.c" \
    "$SAND_DIR/sand.c" \
    "$SAND_DIR/sand_core1.c" \
    "$SAND_DIR/sand_impulse.c" \
    "$SAND_DIR/sand_reactions.c" \
    "$SAND_DIR/sand_plants.c" \
    "$SAND_DIR/sand_gas.c" \
    "$SAND_DIR/sand_liquid.c" \
    "$SAND_DIR/material.c" \
    "$SAND_DIR/material_palette.c" \
    -o "$OUT_BIN" -lm

[ -x "$OUT_BIN" ] || OUT_BIN="$OUT_BIN.exe"

"$OUT_BIN" "$RESULTS_DIR"

#!/bin/sh
#
# Build and run serial_lag.c: how far a two-core step falls behind a serial
# one, per scene, cell for cell.
#
# The split reorders work at a chunk boundary and defers every long-reach
# trigger; that only matters where serial would have done something different
# in the same step. This prints that difference, so a partition change can be
# judged by whether the numbers fall rather than by how a pour looks - see
# serial_lag.c's own top comment.
#
# Usage:
#   main/apps/sand/tools/report_serial_lag.sh
#
set -eu

# This file lives at main/apps/sand/tools/, four levels below launcher/ -
# tools -> sand -> apps -> main -> launcher - resolved the same way as
# report_reactions.sh and report_performance.sh beside it.
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
SAND_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
MAIN_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/../../.." && pwd)
LAUNCHER_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/../../../.." && pwd)

BUILD_DIR="$SCRIPT_DIR/build"

# --- find a compiler -------------------------------------------------------
# Sourced, not copied - see tools/find_cc.sh's own top comment.
# shellcheck source=../../../../tools/find_cc.sh
. "$LAUNCHER_DIR/tools/find_cc.sh"

if ! CC_BIN=$(find_cc); then
    echo "No C compiler found." >&2
    echo "  Windows: winget install BrechtSanders.WinLibs.POSIX.UCRT" >&2
    echo "  Debian:  sudo apt install build-essential" >&2
    echo "  macOS:   xcode-select --install" >&2
    exit 1
fi

# Same flags as run_tests.sh, deliberately copied rather than relaxed: this
# tool's output is used to accept or reject changes, so it has no business
# being built more loosely than the tests whose verdict it supplements.
# -Wno-unused-parameter is part of that set, not a concession made here -
# the app's own sources do not build without it.
CFLAGS="-std=c11 -Wall -Wextra -Werror -Wno-unused-parameter -g -O1"

mkdir -p "$BUILD_DIR"
OUT_BIN="$BUILD_DIR/serial_lag"

# The portable half of the app only. app_sand.c and sand_ui.c are the
# hardware-facing entry points (the apps/<name>/app_*.c convention in
# docs/Building-an-App.md) and do not belong in a host build;
# palette.c and row_runs.c
# are draw-path concerns the grid state does not depend on.
# shellcheck disable=SC2086
"$CC_BIN" $CFLAGS -I "$MAIN_DIR" -I "$SAND_DIR" \
    "$SCRIPT_DIR/serial_lag.c" \
    "$MAIN_DIR/util/job.c" \
    "$SAND_DIR/sand.c" \
    "$SAND_DIR/sand_chunk_sched.c" \
    "$SAND_DIR/sand_impulse.c" \
    "$SAND_DIR/sand_reactions.c" \
    "$SAND_DIR/sand_plants.c" \
    "$SAND_DIR/sand_gas.c" \
    "$SAND_DIR/sand_liquid.c" \
    "$SAND_DIR/material.c" \
    -o "$OUT_BIN"

# MinGW appends .exe; elsewhere the plain name is produced.
[ -x "$OUT_BIN" ] || OUT_BIN="$OUT_BIN.exe"

"$OUT_BIN"

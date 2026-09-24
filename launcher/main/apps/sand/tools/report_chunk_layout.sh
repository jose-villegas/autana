#!/bin/sh
#
# Build and run chunk_layout.c: how evenly each candidate chunk layout
# divides a board's work, per quality grid, scene and gravity class, with a
# shortlist of sides per quality for the QEMU sweep to measure for real.
#
# Host-only and deterministic - no device, no clock, no wall time anywhere in
# the output. See chunk_layout.c's own top comment for what the numbers are
# and, more to the point, what they are not.
#
# Usage:
#   main/apps/sand/tools/report_chunk_layout.sh [OUT.md]
#
#   OUT.md   markdown report path. Default:
#            main/apps/sand/tools/results/chunk_layout_<timestamp>.md

set -eu

# tools -> sand -> apps -> main -> launcher, resolved the same way as
# report_fingerprint.sh beside it.
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
SAND_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
MAIN_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/../../.." && pwd)
LAUNCHER_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/../../../.." && pwd)

BUILD_DIR="$SCRIPT_DIR/build"
REPORT_DIR="$SCRIPT_DIR/results"

# shellcheck source=../../../../tools/find_cc.sh
. "$LAUNCHER_DIR/tools/find_cc.sh"

if ! CC_BIN=$(find_cc); then
    echo "No C compiler found." >&2
    echo "  Windows: winget install BrechtSanders.WinLibs.POSIX.UCRT" >&2
    echo "  Debian:  sudo apt install build-essential" >&2
    echo "  macOS:   xcode-select --install" >&2
    exit 1
fi

# Same flags as run_tests.sh, for the reason report_fingerprint.sh copies
# them: output that decides something is not built more loosely than the
# tests it supplements. -O2 because this runs thousands of boards.
CFLAGS="-std=c11 -Wall -Wextra -Werror -Wno-unused-parameter -g -O2"

mkdir -p "$BUILD_DIR" "$REPORT_DIR"
OUT_BIN="$BUILD_DIR/chunk_layout"

# The portable half of the app only - see report_fingerprint.sh. Named one by
# one rather than globbed: a file appearing under apps/sand/ is not by itself
# a reason for this tool to link it.
# shellcheck disable=SC2086
"$CC_BIN" $CFLAGS -I "$MAIN_DIR" -I "$SAND_DIR" \
    -I "$LAUNCHER_DIR/test" -I "$LAUNCHER_DIR/test/framework" \
    "$SCRIPT_DIR/chunk_layout.c" \
    "$SAND_DIR/tests/suite_sand_scenes.c" \
    "$SAND_DIR/tests/suite_sand_common.c" \
    "$LAUNCHER_DIR/test/framework/unity.c" \
    "$LAUNCHER_DIR/test/suites.c" \
    "$MAIN_DIR/util/job.c" \
    "$SAND_DIR/sand.c" \
    "$SAND_DIR/sand_chunk_sched.c" \
    "$SAND_DIR/sand_impulse.c" \
    "$SAND_DIR/sand_reactions.c" \
    "$SAND_DIR/sand_plants.c" \
    "$SAND_DIR/sand_gas.c" \
    "$SAND_DIR/sand_liquid.c" \
    "$SAND_DIR/material.c" \
    -lm -o "$OUT_BIN"

# MinGW appends .exe; elsewhere the plain name is produced.
[ -x "$OUT_BIN" ] || OUT_BIN="$OUT_BIN.exe"

OUT_MD="${1:-$REPORT_DIR/chunk_layout_$(date +%Y%m%d_%H%M%S).md}"
"$OUT_BIN" > "$OUT_MD"
echo "chunk layout report: $OUT_MD"

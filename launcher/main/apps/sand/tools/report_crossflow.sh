#!/bin/sh
# Host-only pass timing. Dispatch runs inline; these are overhead and work counts, not a two-core speedup.
set -eu
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
SAND_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
LAUNCHER_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/../../../.." && pwd)
. "$LAUNCHER_DIR/tools/find_cc.sh"
CC_BIN=$(find_cc)
BUILD_DIR="$SCRIPT_DIR/build"
mkdir -p "$BUILD_DIR"
"$CC_BIN" -std=c11 -Wall -Wextra -Werror -Wno-unused-parameter -g -O1 \
    -I "$LAUNCHER_DIR/main" -I "$SAND_DIR" -I "$LAUNCHER_DIR/test" -I "$LAUNCHER_DIR/test/framework" \
    "$SCRIPT_DIR/crossflow_bench.c" "$SAND_DIR/tests/suite_sand_scenes.c" "$SAND_DIR/tests/suite_sand_common.c" \
    "$LAUNCHER_DIR/test/framework/unity.c" "$LAUNCHER_DIR/test/suites.c" \
    "$SAND_DIR/sand.c" "$SAND_DIR/sand_chunk_sched.c" "$LAUNCHER_DIR/main/util/job.c" "$SAND_DIR/sand_impulse.c" \
    "$SAND_DIR/sand_reactions.c" "$SAND_DIR/sand_plants.c" "$SAND_DIR/sand_gas.c" \
    "$SAND_DIR/sand_liquid.c" "$SAND_DIR/material.c" \
    -Wl,--wrap=sand_step_liquids -lm -o "$BUILD_DIR/crossflow_bench"
OUT="$BUILD_DIR/crossflow_bench"
[ -x "$OUT" ] || OUT="$OUT.exe"
exec "$OUT"

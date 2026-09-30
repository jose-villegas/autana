#!/bin/sh
#
# Build and run sponza_triangle_sizes.c: how many of Sponza's drawn triangles
# cover 0, 1, 2-4 or more pixel centres at the perf suite's poses.
#
# Usage:
#   main/apps/render_lab/tools/report_sponza_triangle_sizes.sh [--write DIR | --against DIR]
#
#   --write DIR    also keep each pose's frame in DIR
#   --against DIR  also compare each pose's frame with the one kept in DIR,
#                  pixel by pixel - run --write on the base first

set -eu

# tools -> render_lab -> apps -> main -> launcher.
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
APP_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
MAIN_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/../../.." && pwd)
LAUNCHER_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/../../../.." && pwd)

# shellcheck source=../../../../tools/build/find_cc.sh
. "$LAUNCHER_DIR/tools/build/find_cc.sh"
if ! CC_BIN=$(find_cc); then
    echo "No C compiler found." >&2
    exit 1
fi

mode=""
dir=""
case "${1:-}" in
--write) mode="--write" dir="${2:?--write needs a directory}" ;;
--against) mode="--against" dir="${2:?--against needs a directory}" ;;
"") ;;
*) echo "usage: $0 [--write DIR | --against DIR]" >&2; exit 2 ;;
esac

BUILD_DIR="$SCRIPT_DIR/build"
mkdir -p "$BUILD_DIR"
OUT_BIN="$BUILD_DIR/sponza_triangle_sizes"
# The same flags as run_tests.sh; -O2 because it draws a whole flythrough.
"$CC_BIN" -std=c11 -Wall -Wextra -Werror -Wno-unused-parameter -O2 -I "$MAIN_DIR" -I "$LAUNCHER_DIR/test" \
    -I "$LAUNCHER_DIR/components/small3dlib/include" \
    "$SCRIPT_DIR/sponza_triangle_sizes.c" \
    "$APP_DIR/sponza_flythrough.c" \
    "$APP_DIR/sponza_mesh_generated.c" \
    "$MAIN_DIR/render/r3d_lit_pipeline.c" \
    "$MAIN_DIR/render/r3d_path.c" \
    "$MAIN_DIR/render/r3d_span.c" \
    -lm -o "$OUT_BIN"
[ -x "$OUT_BIN" ] || OUT_BIN="$OUT_BIN.exe"

if [ -n "$mode" ]; then
    mkdir -p "$dir"
    "$OUT_BIN" "$mode" "$dir"
else
    "$OUT_BIN"
fi

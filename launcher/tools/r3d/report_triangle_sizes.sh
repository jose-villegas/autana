#!/bin/sh
#
# Build and run triangle_sizes: how many of a lit mesh's drawn triangles
# cover 0, 1, 2-4 or more pixel centres at each pose of a poses file (the
# format is in triangle_sizes.h).
#
# Usage:
#   launcher/tools/r3d/report_triangle_sizes.sh --mesh SOURCE.c:SYMBOL POSES|- [--no-cones] [--write DIR | --against DIR]
#
#   --mesh SOURCE.c:SYMBOL  the baked mesh: the C file that defines it and its r3d_lit_mesh_t symbol
#   POSES                   the poses file: size, lens and one line per pose; - reads standard input
#   --no-cones              cull no cluster by the direction it faces, to compare against
#   --write DIR             also keep each pose's frame in DIR
#   --against DIR           also compare each pose's frame with the one kept in DIR, pixel by pixel

set -eu

# r3d -> tools -> launcher.
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
LAUNCHER_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)
MAIN_DIR="$LAUNCHER_DIR/main"

usage() {
    echo "usage: $0 --mesh SOURCE.c:SYMBOL POSES|- [--no-cones] [--write DIR | --against DIR]" >&2
    exit 2
}

[ "${1:-}" = --mesh ] && [ $# -ge 3 ] || usage
case "$2" in
*.c:?*) ;;
*) usage ;;
esac
mesh_source=${2%:*}
mesh_symbol=${2##*:}
poses=$3
shift 3
[ -f "$mesh_source" ] || { echo "no mesh source $mesh_source" >&2; exit 2; }
[ "$poses" = - ] || [ -f "$poses" ] || { echo "no poses file $poses" >&2; exit 2; }
cones=""
if [ "${1:-}" = --no-cones ]; then
    cones=--no-cones
    shift
fi
mode=""
dir=""
case "${1:-}" in
--write | --against) mode=$1 dir="${2:?$1 needs a directory}" ;;
"") ;;
*) usage ;;
esac

# shellcheck source=../build/find_cc.sh
. "$LAUNCHER_DIR/tools/build/find_cc.sh"
if ! CC_BIN=$(find_cc); then
    echo "No C compiler found." >&2
    exit 1
fi

BUILD_DIR="$SCRIPT_DIR/build"
mkdir -p "$BUILD_DIR"
OUT_BIN="$BUILD_DIR/triangle_sizes"
# The same flags as run_tests.sh; -O2 because it draws every pose.
"$CC_BIN" -std=c11 -Wall -Wextra -Werror -Wno-unused-parameter -O2 \
    -I "$MAIN_DIR" -I "$SCRIPT_DIR" -I "$(dirname -- "$mesh_source")" \
    -I "$LAUNCHER_DIR/components/small3dlib/include" \
    -DR3D_SIZES_MESH="$mesh_symbol" \
    "$SCRIPT_DIR/triangle_sizes_main.c" \
    "$SCRIPT_DIR/triangle_sizes.c" \
    "$mesh_source" \
    "$MAIN_DIR/render/r3d_lit_pipeline.c" \
    "$MAIN_DIR/render/r3d_span.c" \
    -lm -o "$OUT_BIN"
[ -x "$OUT_BIN" ] || OUT_BIN="$OUT_BIN.exe"

if [ -n "$mode" ]; then
    mkdir -p "$dir"
    "$OUT_BIN" "$poses" ${cones:+"$cones"} "$mode" "$dir"
else
    "$OUT_BIN" "$poses" ${cones:+"$cones"}
fi

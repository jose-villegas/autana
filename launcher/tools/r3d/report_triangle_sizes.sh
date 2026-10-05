#!/bin/sh
#
# Build and run triangle_sizes: how many of a lit mesh's drawn triangles
# cover 0, 1, 2-4 or more pixel centres at each pose of a poses file (the
# format is in triangle_sizes.h), and the bounding boxes of the triangles
# the draw hands the rasterizer, by shading mode.
#
# Usage:
#   launcher/tools/r3d/report_triangle_sizes.sh --mesh NAME POSES|- [--write DIR | --against DIR]
#
#   --mesh NAME             the baked mesh: its asset id, read from the bundle that holds it, in a folder
#                           written from the baked meshes in the tree, or the one AUTANA_ASSET_DIR names
#   POSES                   the poses file: size, lens and one line per pose; - reads standard input
#   --write DIR             also keep each pose's frame in DIR
#   --against DIR           also compare each pose's frame with the one kept in DIR, pixel by pixel

set -eu

# r3d -> tools -> launcher.
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
LAUNCHER_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)
MAIN_DIR="$LAUNCHER_DIR/main"

usage() {
    echo "usage: $0 --mesh NAME POSES|- [--write DIR | --against DIR]" >&2
    exit 2
}

[ "${1:-}" = --mesh ] && [ $# -ge 3 ] || usage
mesh_name=$2
poses=$3
shift 3
asset_dir=${AUTANA_ASSET_DIR:-}
[ "$poses" = - ] || [ -f "$poses" ] || { echo "no poses file $poses" >&2; exit 2; }
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
CFLAGS="-std=c11 -Wall -Wextra -Werror -Wno-unused-parameter -O2"
# The pipeline hands its triangles to the tool, which counts their boxes
# and passes them on to r3d_span's own functions.
# shellcheck disable=SC2086 # CFLAGS is a list of flags
"$CC_BIN" $CFLAGS -I "$MAIN_DIR" \
    -Dr3d_span_triangle=sizes_span_triangle -Dr3d_span_triangle_solid=sizes_span_triangle_solid \
    -c "$MAIN_DIR/render/r3d_pipeline.c" -o "$BUILD_DIR/r3d_pipeline_counted.o"
# shellcheck disable=SC2086
"$CC_BIN" $CFLAGS \
    -I "$MAIN_DIR" -I "$SCRIPT_DIR" \
    "$SCRIPT_DIR/triangle_sizes_main.c" \
    "$SCRIPT_DIR/triangle_sizes.c" \
    "$MAIN_DIR/asset/asset_pack.c" \
    "$MAIN_DIR/asset/asset_file.c" \
    "$MAIN_DIR/render/r3d_lit_mesh.c" \
    "$BUILD_DIR/r3d_pipeline_counted.o" \
    "$MAIN_DIR/render/r3d_span.c" \
    -lm -o "$OUT_BIN"
[ -x "$OUT_BIN" ] || OUT_BIN="$OUT_BIN.exe"

# shellcheck source=../../../scripts/lib/python.sh
. "$LAUNCHER_DIR/../scripts/lib/python.sh"
PYTHON=$(find_python) || exit 1
if [ -z "$asset_dir" ]; then
    asset_dir="$BUILD_DIR/assets"
    "$PYTHON" "$SCRIPT_DIR/build_pack.py" -o "$asset_dir" "$MAIN_DIR" > /dev/null
fi
pack="$asset_dir/$("$PYTHON" "$SCRIPT_DIR/build_pack.py" --bundle-of "$mesh_name" "$MAIN_DIR").apak"

if [ -n "$mode" ]; then
    mkdir -p "$dir"
    "$OUT_BIN" "$pack" "$mesh_name" "$poses" "$mode" "$dir"
else
    "$OUT_BIN" "$pack" "$mesh_name" "$poses"
fi

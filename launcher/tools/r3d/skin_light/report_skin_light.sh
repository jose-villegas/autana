#!/bin/sh
#
# Measure skinned-mesh lighting on the host and regenerate its document: the
# cost, table-build and quality tables in docs/render/Skinned-Lighting.md and
# the sheet beside it.
#
# Usage:
#   launcher/tools/r3d/skin_light/report_skin_light.sh ASSET.glb
#
#   ASSET.glb   a skinned glTF with normals and animations; the document's
#               numbers come from a glTF export of
#               main/apps/render_lab/assets/capybara.blend

set -eu

# skin_light -> r3d -> tools -> launcher.
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
LAUNCHER_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/../../.." && pwd)
ROOT_DIR=$(CDPATH= cd -- "$LAUNCHER_DIR/.." && pwd)

[ $# -eq 1 ] && [ -f "$1" ] || { echo "usage: $0 ASSET.glb" >&2; exit 2; }
asset=$1

# shellcheck source=../../build/find_cc.sh
. "$LAUNCHER_DIR/tools/build/find_cc.sh"
CC_BIN=$(find_cc) || { echo "No C compiler found." >&2; exit 1; }
# shellcheck source=../../../../scripts/lib/python.sh
. "$ROOT_DIR/scripts/lib/python.sh"
PYTHON=$(find_python PIL) || exit 1

BUILD_DIR="$SCRIPT_DIR/build"
mkdir -p "$BUILD_DIR/tables"
OUT_BIN="$BUILD_DIR/skin_light_bench"
# -fno-tree-vectorize: the board's FPU is scalar, so the host stays scalar too.
"$CC_BIN" -std=c11 -Wall -Wextra -Werror -Wno-unused-parameter -O2 -fno-tree-vectorize \
    -I "$LAUNCHER_DIR/main" "$SCRIPT_DIR/skin_light_bench.c" -lm -o "$OUT_BIN"
[ -x "$OUT_BIN" ] || OUT_BIN="$OUT_BIN.exe"

cd "$LAUNCHER_DIR"
"$PYTHON" tools/r3d/skin_light/skin_light_data.py "$asset" "$BUILD_DIR/data.bin"
"$OUT_BIN" "$BUILD_DIR/data.bin" "$BUILD_DIR/tables"
mv "$BUILD_DIR/tables/sheet.bin" "$BUILD_DIR/sheet.bin"
"$PYTHON" tools/r3d/skin_light/skin_light_sheet.py "$asset" "$BUILD_DIR/sheet.bin" \
    "$ROOT_DIR/docs/render/images/skin-light-sheet.png"
"$PYTHON" tools/render/generated_blocks.py --root "$ROOT_DIR" --tables "$BUILD_DIR/tables"

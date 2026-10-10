#!/bin/sh
#
# Measure skinned-mesh lighting on the host and regenerate its document: the
# cost, table-build and quality tables in docs/render/Skinned-Lighting.md and
# the sheet beside it.
#
# Usage:
#   launcher/tools/r3d/skin_light/report_skin_light.sh ASSET.glb [CLIP[:PHASE]]
#
#   ASSET.glb      a skinned glTF with normals and animations; the document's
#                  numbers come from the capybara's export, the cached bake
#                  `bake/bake.py path capybara.glb` prints, drawn at gallop
#   CLIP[:PHASE]   the frame the sheet draws (skin_light_data.py's --sheet)

set -eu

# skin_light -> r3d -> tools -> launcher.
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
LAUNCHER_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/../../.." && pwd)
ROOT_DIR=$(CDPATH= cd -- "$LAUNCHER_DIR/.." && pwd)

[ $# -ge 1 ] && [ $# -le 2 ] && [ -f "$1" ] || { echo "usage: $0 ASSET.glb [CLIP[:PHASE]]" >&2; exit 2; }
# Absolute, since the steps below run from launcher/.
asset=$(CDPATH= cd -- "$(dirname -- "$1")" && pwd)/$(basename -- "$1")
sheet=${2:-}

# shellcheck source=../../build/find_cc.sh
. "$LAUNCHER_DIR/tools/build/find_cc.sh"
# shellcheck source=../../build/packages.sh
. "$LAUNCHER_DIR/tools/build/packages.sh"
CC_BIN=$(find_cc) || { echo "No C compiler found." >&2; exit 1; }
# shellcheck source=../../../../scripts/lib/python.sh
. "$ROOT_DIR/scripts/lib/python.sh"
PYTHON=$(find_python PIL) || exit 1

BUILD_DIR="$SCRIPT_DIR/build"
mkdir -p "$BUILD_DIR/tables"
OUT_BIN="$BUILD_DIR/skin_light_bench"
# -fno-tree-vectorize: the board's FPU is scalar, so the host stays scalar too.
"$CC_BIN" -std=c11 -Wall -Wextra -Werror -Wno-unused-parameter -O2 -fno-tree-vectorize \
    -I "$LAUNCHER_DIR/main" $(package_includes "$LAUNCHER_DIR") "$SCRIPT_DIR/skin_light_bench.c" -lm -o "$OUT_BIN"
[ -x "$OUT_BIN" ] || OUT_BIN="$OUT_BIN.exe"

cd "$LAUNCHER_DIR"
"$PYTHON" tools/r3d/skin_light/skin_light_data.py "$asset" "$BUILD_DIR/data.bin" ${sheet:+--sheet "$sheet"}
"$OUT_BIN" "$BUILD_DIR/data.bin" "$BUILD_DIR"
# The tables directory holds only the generated tables generated_blocks.py splices.
mv "$BUILD_DIR"/skin-light-*.md "$BUILD_DIR/tables/"
"$PYTHON" tools/r3d/skin_light/skin_light_sheet.py "$asset" "$BUILD_DIR" \
    "$ROOT_DIR/docs/render/images/skin-light-sheet.png" ${sheet:+--sheet "$sheet"}
"$PYTHON" tools/render/generated_blocks.py --root "$ROOT_DIR" --tables "$BUILD_DIR/tables"

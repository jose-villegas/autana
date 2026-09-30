#!/bin/sh
#
# Print the Sponza flythrough as a poses file, for tools/r3d's
# report_triangle_sizes.sh to read from a pipe. Nothing is written to disk,
# so the poses are always the flythrough's own.
#
#   main/apps/render_lab/tools/gen_sponza_poses.sh | launcher/tools/r3d/report_triangle_sizes.sh --mesh ... -

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

BUILD_DIR="$SCRIPT_DIR/build"
mkdir -p "$BUILD_DIR"
OUT_BIN="$BUILD_DIR/gen_sponza_poses"
"$CC_BIN" -std=c11 -Wall -Wextra -Werror -Wno-unused-parameter -O2 -I "$MAIN_DIR" \
    -I "$LAUNCHER_DIR/components/small3dlib/include" \
    "$SCRIPT_DIR/gen_sponza_poses.c" "$APP_DIR/sponza_flythrough.c" \
    "$MAIN_DIR/render/r3d_lit_pipeline.c" "$MAIN_DIR/render/r3d_path.c" "$MAIN_DIR/render/r3d_span.c" \
    -lm -o "$OUT_BIN"
[ -x "$OUT_BIN" ] || OUT_BIN="$OUT_BIN.exe"
"$OUT_BIN"

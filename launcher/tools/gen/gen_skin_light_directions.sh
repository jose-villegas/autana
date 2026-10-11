#!/bin/sh
# Compile the math-owned direction decoder on the host.
set -eu
GEN_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
LAUNCHER_DIR=$(CDPATH= cd -- "$GEN_DIR/../.." && pwd)
. "$LAUNCHER_DIR/tools/build/find_cc.sh"
CC_BIN=$(find_cc)
GEN_TMP=$(mktemp -d)
trap 'rm -f "$GEN_TMP/directions.exe"; rmdir "$GEN_TMP"' EXIT HUP INT TERM
"$CC_BIN" -std=c11 -Wall -Wextra -Werror -ffp-contract=off \
    -I "$LAUNCHER_DIR/main" -I "$LAUNCHER_DIR/packages/math/include" \
    "$GEN_DIR/gen_skin_light_directions.c" -lm -o "$GEN_TMP/directions.exe"
"$GEN_TMP/directions.exe"

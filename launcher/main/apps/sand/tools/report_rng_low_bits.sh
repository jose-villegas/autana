#!/bin/sh
#
# Build and run rng_low_bits.c: whether masking the low bits of a draw
# gives correlated rolls, and whether the sealed gas pocket that raised
# the question is explained by the generator or by the scene.
#
# Everything here is host-only and deterministic - no device, no clock.
# See rng_low_bits.c's own top comment for what each section measures.
#
# Usage:
#   main/apps/sand/tools/report_rng_low_bits.sh

set -eu

# This file lives at main/apps/sand/tools/, four levels below launcher/ -
# tools -> sand -> apps -> main -> launcher - resolved the same way as
# report_fingerprint.sh beside it.
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
SAND_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
MAIN_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/../../.." && pwd)
LAUNCHER_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/../../../.." && pwd)

BUILD_DIR="$SCRIPT_DIR/build"

# shellcheck source=../../../../tools/find_cc.sh
. "$LAUNCHER_DIR/tools/find_cc.sh"

if ! CC_BIN=$(find_cc); then
    echo "No C compiler found." >&2
    echo "  Windows: winget install BrechtSanders.WinLibs.POSIX.UCRT" >&2
    echo "  Debian:  sudo apt install build-essential" >&2
    echo "  macOS:   xcode-select --install" >&2
    exit 1
fi

# Same flags as run_tests.sh, for the same reason report_fingerprint.sh
# copies them: a tool whose output decides something has no business
# being built more loosely than the tests it supplements. -O2 here
# because the stream sections draw tens of millions of values.
CFLAGS="-std=c11 -Wall -Wextra -Werror -Wno-unused-parameter -g -O2"

mkdir -p "$BUILD_DIR"
OUT_BIN="$BUILD_DIR/rng_low_bits"

# The portable half of the app only - see report_fingerprint.sh.
# shellcheck disable=SC2086
"$CC_BIN" $CFLAGS -I "$MAIN_DIR" -I "$SAND_DIR" \
    "$SCRIPT_DIR/rng_low_bits.c" \
    "$MAIN_DIR/util/job.c" \
    "$SAND_DIR/sand.c" \
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

#!/bin/sh
#
# Build and run grid_fingerprint.c: a hash of the simulation's actual
# output per reference scene, with the material histogram behind it.
#
# Normal and host-only SAND_FORCE_WORK builds share one baseline. Forced
# work evaluates every SAND_SKIP_IF condition but never takes its skip.
# Coverage counts how often each source site would have skipped; zero is
# an untested-skip warning. Counts stay separate from the hash output.
#
# Usage:
#   main/apps/sand/tools/report_fingerprint.sh            # print
#   main/apps/sand/tools/report_fingerprint.sh --check    # diff vs baseline
#   main/apps/sand/tools/report_fingerprint.sh --update   # re-record baseline
#
# --check compares both builds; --update records the normal build only.
#
# --update rewrites the baseline and is DELIBERATELY not something an
# automated script may call. A loop that can re-record its own baseline
# has no baseline; recording one is a human act, done when a behavioural
# change has been reviewed and accepted.

set -eu

# This file lives at main/apps/sand/tools/, four levels below launcher/ -
# tools -> sand -> apps -> main -> launcher - resolved the same way as
# report_reactions.sh and report_performance.sh beside it.
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
SAND_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
MAIN_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/../../.." && pwd)
LAUNCHER_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/../../../.." && pwd)

BASELINE="$SCRIPT_DIR/fingerprint_baseline.txt"
BUILD_DIR="$SCRIPT_DIR/build"

# --- find a compiler -------------------------------------------------------
# Sourced, not copied - see tools/build/find_cc.sh's own top comment.
# shellcheck source=../../../../tools/build/find_cc.sh
. "$LAUNCHER_DIR/tools/build/find_cc.sh"

if ! CC_BIN=$(find_cc); then
    echo "No C compiler found." >&2
    echo "  Windows: winget install BrechtSanders.WinLibs.POSIX.UCRT" >&2
    echo "  Debian:  sudo apt install build-essential" >&2
    exit 1
fi

# Same flags as run_tests.sh, deliberately copied rather than relaxed: this
# tool's output is used to accept or reject changes, so it has no business
# being built more loosely than the tests whose verdict it supplements.
# -Wno-unused-parameter is part of that set, not a concession made here -
# the app's own sources do not build without it.
CFLAGS="-std=c11 -Wall -Wextra -Werror -Wno-unused-parameter -g -O1"

mkdir -p "$BUILD_DIR"
for MODE in normal forced; do
    OUT_BIN="$BUILD_DIR/grid_fingerprint"
    FORCE_FLAG=""
    if [ "$MODE" = forced ]; then
        OUT_BIN="${OUT_BIN}_forced"
        FORCE_FLAG="-DSAND_FORCE_WORK"
    fi

    # The portable half of the app only. app_sand.c and sand_ui.c are the
    # hardware-facing entry points (the apps/<name>/app_*.c convention in
    # docs/Building-an-App.md) and do not belong in a host build;
    # palette.c and row_runs.c
    # are draw-path concerns the grid state does not depend on.
    # shellcheck disable=SC2086
    "$CC_BIN" $CFLAGS $FORCE_FLAG -I "$MAIN_DIR" -I "$SAND_DIR" \
        "$SCRIPT_DIR/grid_fingerprint.c" \
        "$MAIN_DIR/util/runtime/job.c" \
        "$SAND_DIR/sand.c" \
        "$SAND_DIR/sand_chunk_sched.c" \
        "$SAND_DIR/sand_impulse.c" \
        "$SAND_DIR/sand_reactions.c" \
        "$SAND_DIR/sand_plants.c" \
        "$SAND_DIR/sand_gas.c" \
        "$SAND_DIR/sand_liquid.c" \
        "$SAND_DIR/material.c" \
        -o "$OUT_BIN"

    # MinGW appends .exe; elsewhere the plain name is produced.
    [ -x "$OUT_BIN" ] || OUT_BIN="$OUT_BIN.exe"
    "$OUT_BIN" > "$BUILD_DIR/fingerprint.$MODE.txt" 2> "$BUILD_DIR/skip.$MODE.txt"
done

python "$MAIN_DIR/../../scripts/gates/check_skip_facts.py" --coverage "$BUILD_DIR/skip.forced.txt" "$SAND_DIR"

case "${1:-}" in
--check)
    if [ ! -f "$BASELINE" ]; then
        echo "No baseline at $BASELINE - record one with --update first." >&2
        exit 1
    fi
    RESULT=0
    for MODE in normal forced; do
        if diff -u "$BASELINE" "$BUILD_DIR/fingerprint.$MODE.txt"; then
            echo "fingerprint ($MODE): identical to baseline"
        else
            RESULT=1
            if [ "$MODE" = forced ]; then
                echo "Forced-work mismatch: a skip dropped needed work." >&2
            else
                echo "BEHAVIOUR CHANGED in the normal build." >&2
            fi
        fi
    done
    exit "$RESULT"
    ;;
--update)
    cp "$BUILD_DIR/fingerprint.normal.txt" "$BASELINE"
    echo "Baseline recorded: $BASELINE"
    ;;
"")
    echo "fingerprint (normal):"
    cat "$BUILD_DIR/fingerprint.normal.txt"
    echo "fingerprint (forced):"
    cat "$BUILD_DIR/fingerprint.forced.txt"
    ;;
*)
    echo "Unknown argument: $1" >&2
    echo "Usage: report_fingerprint.sh [--check|--update]" >&2
    exit 2
    ;;
esac

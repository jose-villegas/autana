#!/bin/sh
#
# Build and run grid_fingerprint.c: a hash of the simulation's actual
# output per reference scene, with the material histogram behind it.
#
# Two builds of the same scenes. The normal build is the program as it
# ships. The forced-work build (-DSAND_FORCE_WORK, host only) never takes a
# SAND_SKIP_IF skip: every skip does its work, and any non-cell state that
# work touches (RNG, pass flips) is put back. A skip whose fact is right
# drops only work that changes no cell, so the two builds must hash the
# same. A skip that drops cell-changing work makes them differ.
#
# What that catches and what it cannot: a fact that lets a skip drop real
# work shows up as forced != normal, in any scene that reaches it, even
# when the baseline was re-recorded with that skip in place. A fact that is
# merely too strict - skipping less than it could - changes no output and
# costs only time; nothing here sees it, and nothing needs to.
#
# Coverage: both builds count, per SAND_SKIP_IF site, how often its skip
# condition held across all scenes (the normal one with -DSAND_COUNT_SKIPS,
# which counts and still skips), and every site in the sources is listed
# with the sum. A site at 0 was never exercised, so the comparison says
# nothing about it, and --check fails until a scene reaches it.
#
# Usage:
#   main/apps/sand/tools/report_fingerprint.sh            # print
#   main/apps/sand/tools/report_fingerprint.sh --check    # diff vs baseline
#   main/apps/sand/tools/report_fingerprint.sh --update   # re-record baseline
#
# --check is the gate: exit 0 means the normal build is byte-identical to
# the baseline AND the forced build is identical to the normal one. It
# prints the diff, so a caller that cannot interpret a hash can still show
# a human which scene changed and whether the material counts moved with
# it (they should not, for a pure reordering).
#
# --update records the normal build, and refuses while forced != normal:
# a baseline must never be recorded with a skip that drops work. It is
# DELIBERATELY not something an automated script may call. A loop that can
# re-record its own baseline has no baseline; recording one is a human act,
# done when a behavioural change has been reviewed and accepted.

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
# shellcheck source=../../../../tools/build/packages.sh
. "$LAUNCHER_DIR/tools/build/packages.sh"

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

# Debian and CI have only python3; a Windows install has only python.
PYTHON=$(command -v python3 || command -v python) || {
    echo "No python found." >&2
    exit 1
}

mkdir -p "$BUILD_DIR"
for MODE in normal forced; do
    OUT_BIN="$BUILD_DIR/grid_fingerprint"
    FORCE_FLAG="-DSAND_COUNT_SKIPS"
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
    "$CC_BIN" $CFLAGS $FORCE_FLAG -I "$MAIN_DIR" $(package_includes "$LAUNCHER_DIR") -I "$SAND_DIR" \
        "$SCRIPT_DIR/grid_fingerprint.c" \
        "$MAIN_DIR/core/job.c" \
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

COVERED=1
"$PYTHON" "$MAIN_DIR/../../scripts/gates/check_skip_facts.py" --coverage "$BUILD_DIR/skip.normal.txt"     --coverage "$BUILD_DIR/skip.forced.txt" "$SAND_DIR" || COVERED=0

# The forced build against the normal one, not against the baseline: a
# skip that drops work makes the normal build differ, while the forced one
# keeps doing that work, so only this comparison names the skip as the
# cause - and it holds even after the baseline has been re-recorded.
forced_matches_normal() {
    if diff -u "$BUILD_DIR/fingerprint.normal.txt" "$BUILD_DIR/fingerprint.forced.txt"; then
        return 0
    fi
    echo >&2
    echo "A SKIP DROPPED WORK. The forced-work build, which never skips," >&2
    echo "differs from the normal build in the scenes above: some" >&2
    echo "SAND_SKIP_IF site skipped work that changes cells, so the fact it" >&2
    echo "rests on is wrong. The coverage table above lists the sites." >&2
    return 1
}

case "${1:-}" in
--check)
    if [ ! -f "$BASELINE" ]; then
        echo "No baseline at $BASELINE - record one with --update first." >&2
        exit 1
    fi
    RESULT=0
    if diff -u "$BASELINE" "$BUILD_DIR/fingerprint.normal.txt"; then
        echo "fingerprint: identical to baseline"
    else
        RESULT=1
        echo >&2
        echo "BEHAVIOUR CHANGED. The simulation no longer produces the same" >&2
        echo "grid it did at the recorded baseline." >&2
        echo >&2
        echo "Read the diff above by the numbers, not just the hash: the 16" >&2
        echo "columns after it are per-material cell counts. Identical counts" >&2
        echo "with a different hash means cells moved but nothing was created" >&2
        echo "or destroyed - the signature of a REORDERING, which this project" >&2
        echo "has found to be semantically fine before (and expensive to prove" >&2
        echo "so). Changed counts mean material appeared or vanished, which is" >&2
        echo "a bug until someone demonstrates otherwise." >&2
    fi
    if forced_matches_normal; then
        echo "fingerprint: forced work identical to normal"
    else
        RESULT=1
    fi
    if [ "$COVERED" -eq 0 ]; then
        RESULT=1
        echo >&2
        echo "AN UNTESTED SKIP. No scene reaches the SAND_SKIP_IF sites marked" >&2
        echo "UNTESTED above, so the forced comparison says nothing about them:" >&2
        echo "add a row to grid_fingerprint.c that does." >&2
    fi
    exit "$RESULT"
    ;;
--update)
    if ! forced_matches_normal; then
        echo "Baseline NOT recorded." >&2
        exit 1
    fi
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

#!/bin/sh
#
# Build and run the portable test suites on this machine.
#
#   ./test/run_tests.sh
#   CC=clang ./test/run_tests.sh
#   ./test/run_tests.sh --verbose
#   ./test/run_tests.sh --build-only     # compile, link and the stack gate; run nothing
#   ./test/run_tests.sh --jobs 4         # parallel compiles (default: half the CPUs, at most 8)
#   ./test/run_tests.sh --sanitize       # with UBSan, and ASan on Linux
#   ./test/run_tests.sh --build-dir DIR  # build here, not in test/build
#
# This is the fast loop: it compiles for THIS machine, not the ESP32, and runs
# in well under a second. Red-green-refactor is only practical with instant
# feedback, and a build-and-flash cycle is about ninety seconds.
#
# It runs only the PORTABLE suites. The hardware ones need real framebuffer
# memory, DMA and I2C, so they live in the firmware and run at boot on the
# device; see main/selftest/selftest.c. The suite sources are shared, so what passes
# here is the same set of assertions the board makes.
#
# Default output is the verdict and test count. A passing run can emit
# thousands of characters; the full stream is in the printed log path.
#
# POSIX sh on purpose: works under Git Bash or MSYS on Windows, and natively
# on Linux.

set -eu

VERBOSE=0
SANITIZE=0
BUILD_DIR=""
BUILD_ONLY=0
JOBS=""
while [ $# -gt 0 ]; do
    case "$1" in
        --verbose) VERBOSE=1 ;;
        --sanitize) SANITIZE=1 ;;
        --build-dir)
            [ $# -ge 2 ] || { echo "--build-dir needs a folder" >&2; exit 2; }
            BUILD_DIR=$2
            shift
            ;;
        --build-only) BUILD_ONLY=1 ;;
        --jobs)
            [ $# -ge 2 ] || { echo "--jobs needs a number" >&2; exit 2; }
            JOBS=$2
            shift
            ;;
        --print-sources|--print-flags) break ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done
export VERBOSE

# shellcheck disable=SC1007
TEST_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
# shellcheck disable=SC1007
MAIN_DIR=$(CDPATH= cd -- "$TEST_DIR/../main" && pwd)
# --build-dir keeps two runs from clobbering each other: the build dir holds
# one host_tests binary, so concurrent runs (two terminals, a sweep script
# running beside a manual run) otherwise race to compile and execute the
# same file, and a result can end up attributed to a source state that never
# existed.
BUILD_DIR="${BUILD_DIR:-$TEST_DIR/build}"

# --- find a compiler -------------------------------------------------------
# Sourced rather than defined here, so that report_reactions.sh (main/apps/
# sand/tools/) can find a compiler the same way without a hand-copied twin;
# see tools/build/find_cc.sh's own top comment.
# shellcheck source=../tools/build/find_cc.sh
. "$TEST_DIR/../tools/build/find_cc.sh"

if ! CC_BIN=$(find_cc); then
    echo "No C compiler found." >&2
    echo "  Windows: winget install BrechtSanders.WinLibs.POSIX.UCRT" >&2
    echo "  Debian:  sudo apt install build-essential" >&2
    exit 1
fi

# Warnings are errors: a host build catches mistakes the target build misses,
# and strictness costs nothing in tests.
# 64-bit pointers and 8-byte alignment make every command bigger on the host.
BASE_CFLAGS="-std=c11 -Wall -Wextra -Werror -Werror=vla -ffp-contract=off -Wno-unused-parameter -g -O1"
CFLAGS="$BASE_CFLAGS"
if [ "$SANITIZE" = 1 ]; then
    # Instrumentation widens the ranges that format-truncation reasons about.
    CFLAGS="$CFLAGS -fsanitize=undefined -fsanitize-recover=undefined -Wno-format-truncation"
    case "$(uname -s)" in
        Linux) CFLAGS="$CFLAGS -fsanitize=address -fno-omit-frame-pointer" ;;
    esac
fi

# --- the device's heap, on this machine ------------------------------------
# Sourced the same way find_cc.sh is, one block above. The cap is a profile
# field rather than a literal here for the reason device_profile.sh's own
# header gives: it is a per-chip number, and a second board may join the
# test family. Selection is $DEVICE_PROFILE, default esp32s3.
# shellcheck source=../tools/device/device_profile.sh
. "$TEST_DIR/../tools/device/device_profile.sh"
device_profile_load "" "$TEST_DIR/../tools/device/device_profiles" || exit 1
HOST_HEAP_ARENA_BYTES=$(device_profile_require DP_FREE_HEAP_BYTES) || exit 1
HOST_HEAP_ARENA_PSRAM_BYTES=$(device_profile_require DP_PSRAM_BYTES) || exit 1
HOST_HEAP_ARENA_ALWAYSINTERNAL_BYTES=$(device_profile_require DP_SPIRAM_ALWAYSINTERNAL_BYTES) || exit 1
# One list for the test binary and --print-flags, so the complexity gate parses
# heap_arena.c with every define the real compile has.
HEAP_ARENA_DEFINES="-DHOST_HEAP_ARENA -DHOST_HEAP_ARENA_BYTES=$HOST_HEAP_ARENA_BYTES -DHOST_HEAP_ARENA_PSRAM_BYTES=$HOST_HEAP_ARENA_PSRAM_BYTES -DHOST_HEAP_ARENA_ALWAYSINTERNAL_BYTES=$HOST_HEAP_ARENA_ALWAYSINTERNAL_BYTES"

SOURCES="
$TEST_DIR/host_main.c
$TEST_DIR/suites.c
$TEST_DIR/timing.c
$TEST_DIR/test_cleanup.c
$TEST_DIR/heap_arena.c
$TEST_DIR/test_asset_dir.c
$TEST_DIR/test_fence.c
$MAIN_DIR/app/app_arena.c
$MAIN_DIR/app/app_registry.c
$MAIN_DIR/shell/shell_system.c
$MAIN_DIR/input/touch_fsm.c
$MAIN_DIR/input/touch_calib.c
$MAIN_DIR/input/touch_point.c
$MAIN_DIR/input/touch_inject_fsm.c
$MAIN_DIR/input/gesture.c
$MAIN_DIR/input/touch_gesture.c
$MAIN_DIR/input/tilt.c
$MAIN_DIR/input/button_fsm.c
$MAIN_DIR/display/display.c
$MAIN_DIR/boot/boot_anim.c
$MAIN_DIR/boot/boot_anim_motion.c
$MAIN_DIR/selftest/post_layout.c
$MAIN_DIR/util/runtime/job.c
$MAIN_DIR/util/runtime/memory.c
$MAIN_DIR/util/runtime/settings_policy.c
$MAIN_DIR/anim/anim_track.c
$MAIN_DIR/anim/anim_tracks.c
$MAIN_DIR/asset/asset_pack.c
$MAIN_DIR/asset/asset_file.c
$MAIN_DIR/asset/asset_directory.c
$MAIN_DIR/asset/asset_store.c
$MAIN_DIR/asset/asset_store_file.c
$MAIN_DIR/render/r3d_lit_mesh.c
$MAIN_DIR/render/raster.c
$MAIN_DIR/render/raster_show.c
$MAIN_DIR/render/raster_motion.c
$MAIN_DIR/render/r3d_pipeline.c
$MAIN_DIR/render/upscale.c
$MAIN_DIR/render/r3d_span.c
$MAIN_DIR/render/r3d_scene.c
$MAIN_DIR/scene/scene.c
$MAIN_DIR/scene/scene_asset.c
$MAIN_DIR/scene/scene_draw.c
$MAIN_DIR/util/runtime/tune.c
$MAIN_DIR/console/console_verbs.c
$MAIN_DIR/display/panel_clock.c
$MAIN_DIR/gfx/gfx.c
$MAIN_DIR/ui/ui.c
$MAIN_DIR/ui/ui_bridge.c
$MAIN_DIR/ui/ui_build.c
$MAIN_DIR/ui/ui_canvas_marks.c
$MAIN_DIR/ui/ui_launcher_draw.c
$MAIN_DIR/ui/ui_pointer.c
$MAIN_DIR/ui/ui_ridge.c
$MAIN_DIR/ui/ui_snap.c
$MAIN_DIR/ui/ui_scroll.c
$MAIN_DIR/ui/ui_widgets.c
$MAIN_DIR/gfx/gfx_palette_standard.c
$MAIN_DIR/../tools/gen/gfx_palette_gen.c
$MAIN_DIR/../tools/r3d/triangle_sizes.c
$TEST_DIR/../components/microui/src/microui.c
"

for suite_src in "$TEST_DIR"/suites/suite_*.c; do
    [ -e "$suite_src" ] || continue
    SOURCES="$SOURCES
$suite_src"
done

# App-owned sources, discovered rather than listed, so adding or deleting an
# app needs no change here.
#
# The convention: inside main/apps/<name>/, the file named app_*.c is the
# hardware-facing entry point; it talks to gfx, the IMU and the frame loop, so
# it cannot link on a host. A scene_*.c is the same kind of file: one of
# several hardware-facing renderers an app hosts behind its single app_*.c.
# Everything else in the folder is portable logic and is
# compiled in, along with the suite_*.c in its tests/ folder.
#
# That split is not bureaucracy: it is what forces an app's logic to be
# separable from its wiring, which is the only reason a falling-sand automaton
# can be tested on a laptop at all.
#
# Recursive, matching main/CMakeLists.txt's own discovered_apps glob and its
# tools/ exclusion: a screen's drawing code lives one level deeper, in
# apps/<name>/ui/, so a one-level walk would silently drop it from this
# runner while the firmware kept building it. apps/<name>/tools/ (sweep
# scripts, report generators) is excluded the same way CMake excludes it:
# by folder, not depth, so a future two-level-deep non-tools folder is swept
# in rather than silently skipped.
for f in $(find "$MAIN_DIR/apps" -name '*.c' ! -path '*/tools/*' | sort); do
    [ -e "$f" ] || continue
    case "$(basename "$f")" in
        app_*.c | scene_*.c) continue ;;
    esac
    SOURCES="$SOURCES
$f"
done

# Exit here, before touching a compiler, for a caller that only wants the
# exact file list or flag set this script proves compilable: the clang-tidy
# complexity gate (tools/quality/complexity_gate.py) builds its compile database
# from these instead of keeping its own copy, so the two cannot drift apart
# the way cognitive_complexity.py's own function finder did. -Werror is
# left out of --print-flags: it is this script's own strictness choice, not
# a fact about what compiles, and a warning unrelated to complexity should
# not cost that file its coverage in the gate.
case "${1:-}" in
    --print-sources)
        printf '%s\n' $SOURCES | sed '/^$/d'
        exit 0
        ;;
    --print-flags)
        printf '%s\n' -std=c11 -Wall -Wextra -Wno-unused-parameter -g -O1 \
            -I "$MAIN_DIR" -I "$TEST_DIR" -I "$TEST_DIR/framework" -I "$TEST_DIR/stubs" \
            -I "$TEST_DIR/../components/microui/include" \
            -I "$TEST_DIR/../tools/gen" -I "$TEST_DIR/../tools/r3d" $HEAP_ARENA_DEFINES \
            -include "$TEST_DIR/timing.h"
        exit 0
        ;;
esac

mkdir -p "$BUILD_DIR"
QUIET_LOG="$BUILD_DIR/run_tests.log"
if [ -z "${QUIET_INNER:-}" ]; then
    # shellcheck source=../../scripts/quiet.sh
    . "$TEST_DIR/../../scripts/quiet.sh"
    quiet_begin "$QUIET_LOG"
    [ "$VERBOSE" = 1 ] && set -- "$@" --verbose
    [ "$SANITIZE" = 1 ] && set -- "$@" --sanitize
    [ "$BUILD_ONLY" = 1 ] && set -- "$@" --build-only
    [ -n "$JOBS" ] && set -- "$@" --jobs "$JOBS"
    quiet_run host-tests env QUIET_INNER=1 sh "$0" "$@" --build-dir "$BUILD_DIR" || true
    QUIET_SUMMARY=$(grep -E '^[0-9]+ Tests [0-9]+ Failures [0-9]+ Ignored' "$QUIET_LOG" | tail -n 1)
    export QUIET_SUMMARY
    QUIET_FAILURES=$(grep -E ':FAIL|ERROR: (AddressSanitizer|LeakSanitizer)' "$QUIET_LOG" || true)
    if [ -n "$QUIET_FAILURES" ]; then
        printf 'Test and sanitizer failures:\n%s\n' "$QUIET_FAILURES"
    fi
    if [ "$SANITIZE" = 1 ]; then
        QUIET_FINDINGS=$(grep 'runtime error:' "$QUIET_LOG" | sed -E 's/:[0-9]+: runtime error:/: runtime error:/' | sort -u || true)
        if [ -n "$QUIET_FINDINGS" ]; then
            printf 'UBSan findings (%s):\n%s\n' "$(printf '%s\n' "$QUIET_FINDINGS" | wc -l | tr -d ' ')" "$QUIET_FINDINGS"
            quiet_end run_tests 1 || exit $?
        fi
    fi
    quiet_end run_tests || exit $?
    exit 0
fi

# The hardware-facing app_*.c files are excluded from SOURCES above because
# they cannot link here, which also meant nothing compiled them at all
# until a full device build. Compile-check them first, so a change that
# does not build is caught here rather than on the board.
if [ "$BUILD_ONLY" != 1 ]; then
    "$TEST_DIR/check_app_sources.sh"
    "$TEST_DIR/check_inline_owners.sh"

    # The bootloader hook lives outside SOURCES too: a separate header world
    # entirely, so it gets its own standalone binary rather than joining the
    # suites above.
    "$TEST_DIR/check_pmic_cold_boot.sh" --build-dir "$BUILD_DIR"
fi

# --- incremental build ------------------------------------------------------
# One object per translation unit, built by GNU make from a Makefile this
# script writes into the build dir: make already does exactly the two things
# this needs (mtime comparison and -MMD depfile tracking) and runs jobs in
# parallel, and it exists on both platforms this repo builds on (make on
# Linux, mingw32-make beside WinLibs' gcc). A sh loop would have to
# re-implement depfile parsing.
#
# Nothing but the compiler's own facts decides staleness: a header edit
# rebuilds its includers through the depfiles, and every flag that shapes
# an object (compiler, flags, defines, includes) is written into a stamp file
# that all objects depend on; a change rebuilds everything, and a sanitizer
# build keeps its objects in a directory of its own so switching does not
# thrash.
#
# unity.c must NOT see the -include timing.h that every other source gets:
# Unity's RUN_TEST is guarded by "#ifndef RUN_TEST", and if timing.h has
# already defined it, Unity assumes a replacement runner exists and compiles
# UnityDefaultTestRun (the one function timing.c calls) out entirely.
#
# components/microui/include is on the path for ui_style.h's sake, which needs
# mu_Rect and mu_Color; microui.c itself is linked for the one suite that
# drives the real widget code.
#
# --wrap routes the suite's own allocations into heap_arena.c's device-sized
# arena, so a fixture that asks for more than the board has fails HERE
# rather than after a flash. Only this runner defines HOST_HEAP_ARENA: any
# other build compiling the same timing.c gets every arena line preprocessed
# out. libc-internal allocations do not route through --wrap (a pointer from
# strdup() reaches __wrap_free never having been seen by __wrap_malloc),
# which is why the arena forwards pointers it does not own.
#
# -lm goes last: GNU ld resolves left to right. Only Linux needs it, but a
# suite using atan2() links clean on Windows and fails only in CI otherwise.
# shellcheck source=../tools/build/host_make.sh
. "$TEST_DIR/../tools/build/host_make.sh"
MAKE_BIN=$(find_make) || exit 1
JOBS=$(host_jobs "$JOBS")

# make reads native paths: on Windows it is a native program, so the MSYS
# path rewriting that shields gcc under sh does not apply to it.
if command -v cygpath >/dev/null 2>&1; then
    to_native() { cygpath -m -f -; }
else
    to_native() { cat; }
fi
# Collapses "a/../b" so two spellings of one file get one object name.
squash() { sed -e ':a' -e 's|/[^/][^/]*/\.\./|/|' -e 'ta'; }
native() { printf '%s\n' "$1" | to_native | squash; }

case "$(uname -s)" in
    MINGW* | MSYS* | CYGWIN*) EXE=.exe ;;
    *) EXE= ;;
esac
OUT="$BUILD_DIR/host_tests$EXE"
OUT_N=$(native "$OUT")

CONFIG=plain
[ "$SANITIZE" = 1 ] && CONFIG=ubsan
OBJ_DIR="obj/$CONFIG"
SU_DIR="$BUILD_DIR/su"
mkdir -p "$BUILD_DIR/$OBJ_DIR" "$SU_DIR"

CC_MK=$CC_BIN
case "$CC_BIN" in */* | *\\*) CC_MK=$(native "$CC_BIN") ;; esac
MAIN_N=$(native "$MAIN_DIR")
TEST_N=$(native "$TEST_DIR")
LAUNCHER_N=$(native "$(CDPATH= cd -- "$TEST_DIR/.." && pwd)")
BUILD_N=$(native "$BUILD_DIR")
COMMON_INC="-I $MAIN_N -I $TEST_N -I $TEST_N/framework -I $TEST_N/stubs"
TEST_INC="$COMMON_INC -I $LAUNCHER_N/components/microui/include -I $LAUNCHER_N/tools/gen -I $LAUNCHER_N/tools/r3d"
LDFLAGS="-Wl,--wrap=malloc -Wl,--wrap=calloc -Wl,--wrap=realloc -Wl,--wrap=free -lm"

# The source lists, one native path per line. Test code (test/'s own
# drivers, test/suites/*.c and each app's suite_*.c) also gets the
# -fstack-usage pass below.
SRC_LIST="$BUILD_DIR/sources.txt"
printf '%s\n' $SOURCES | sed '/^$/d' | to_native | squash >"$SRC_LIST"
SU_LIST="$BUILD_DIR/su_sources.txt"
SU_SOURCES=""
for f in $SOURCES; do
    case "$f" in
        "$TEST_DIR"/* | */suite_*.c) SU_SOURCES="$SU_SOURCES $f" ;;
    esac
done
printf '%s\n' $SU_SOURCES | sed '/^$/d' | to_native | squash >"$SU_LIST"

# An object's name is its launcher-relative path with the slashes flattened;
# "name source" pairs feed awk, so the whole Makefile costs a handful of
# process launches (each one is expensive under MSYS).
pairs() { # source-list-file
    sed -e "s|^$LAUNCHER_N/||" -e 's|/|__|g' -e 's|\.c$||' "$1" | paste -d' ' - "$1"
}
OBJ_PAIRS=$(pairs "$SRC_LIST")
SU_PAIRS=$(pairs "$SU_LIST")

MK="$BUILD_DIR/host.mk"
EXPECT_OBJ="$BUILD_DIR/expected_obj.txt"
EXPECT_SU="$BUILD_DIR/expected_su.txt"
{
    printf 'CC := %s\n' "$CC_MK"
    printf 'CFLAGS := %s\n' "$CFLAGS"
    printf 'BASE_CFLAGS := %s\n' "$BASE_CFLAGS"
    printf 'TEST_FLAGS := %s %s -include %s/timing.h\n' "$HEAP_ARENA_DEFINES" "$TEST_INC" "$TEST_N"
    printf 'OBJ := %s\nSU := su\n' "$OBJ_DIR"
    printf 'OBJS :='
    printf '%s\n' "$OBJ_PAIRS" | awk '{ printf " $(OBJ)/%s.o", $1 }'
    printf '\nSUOBJS :='
    printf '%s\n' "$SU_PAIRS" | awk '{ printf " $(SU)/%s.o", $1 }'
    printf '\nall: %s $(SUOBJS)\n' "$OUT_N"
    printf '%s\n' "$OBJ_PAIRS" | awk '{
        printf "$(OBJ)/%s.o: %s $(OBJ)/flags.stamp\n", $1, $2
        printf "\t@echo \"  CC $(notdir $@)\"\n"
        printf "\t@$(CC) $(CFLAGS) $(TEST_FLAGS) -MMD -MP -c $< -o $@\n"
    }'
    printf '$(OBJ)/unity.o: %s/framework/unity.c $(OBJ)/flags.stamp\n' "$TEST_N"
    printf '\t@echo "  CC unity.o"\n'
    printf '\t@$(CC) $(CFLAGS) %s -MMD -MP -c $< -o $@\n' "$COMMON_INC"
    printf '%s: $(OBJS) $(OBJ)/unity.o $(OBJ)/flags.stamp\n' "$OUT_N"
    printf '\t@echo "  LD host_tests"\n'
    printf '\t@$(CC) $(CFLAGS) $(OBJS) $(OBJ)/unity.o -o $@ %s\n' "$LDFLAGS"
    printf '%s\n' "$SU_PAIRS" | awk '{
        printf "$(SU)/%s.o: %s $(SU)/flags.stamp\n", $1, $2
        printf "\t@echo \"  SU $(notdir $@)\"\n"
        printf "\t@$(CC) $(BASE_CFLAGS) $(TEST_FLAGS) -fstack-usage -MMD -MP -c $< -o $@\n"
    }'
    printf -- '-include $(wildcard $(OBJ)/*.d $(SU)/*.d)\n'
} >"$MK"

# Everything that shapes an object goes in a stamp, rewritten only when it
# differs so an unchanged configuration leaves the objects current.
write_stamp() { # file, content
    if [ "$(cat "$1" 2>/dev/null)" != "$2" ]; then printf '%s\n' "$2" >"$1"; fi
}
CC_ID="$("$CC_BIN" --version 2>&1 | sed -n 1p) $CC_MK"
write_stamp "$BUILD_DIR/$OBJ_DIR/flags.stamp" "$CC_ID
$CFLAGS
$HEAP_ARENA_DEFINES $TEST_INC $LDFLAGS"
write_stamp "$SU_DIR/flags.stamp" "$CC_ID
$BASE_CFLAGS
$HEAP_ARENA_DEFINES $TEST_INC"

# Drop objects, depfiles and .su files whose source is gone, so a deleted
# suite cannot leave a stale .su for the stack gate to read.
prune() { # dir, expected-name-list-file
    (cd "$1" && ls -1) | LC_ALL=C sort | LC_ALL=C comm -23 - "$2" | while IFS= read -r f; do
        rm -f "$1/$f"
    done
}
{
    printf '%s\n' "$OBJ_PAIRS" | awk '{ print $1 ".o"; print $1 ".d" }'
    printf '%s\n' unity.o unity.d flags.stamp
} | LC_ALL=C sort >"$EXPECT_OBJ"
{
    printf '%s\n' "$SU_PAIRS" | awk '{ print $1 ".o"; print $1 ".d"; print $1 ".su" }'
    printf '%s\n' flags.stamp
} | LC_ALL=C sort >"$EXPECT_SU"
prune "$BUILD_DIR/$OBJ_DIR" "$EXPECT_OBJ"
prune "$SU_DIR" "$EXPECT_SU"

"$MAKE_BIN" -f "$BUILD_N/host.mk" -C "$BUILD_N" -j "$JOBS"

# A gate that quietly checks nothing is worse than no gate: if -fstack-usage
# is not supported (older compiler, unexpected toolchain), no .su files are
# written at all, so fail loudly here rather than let check_stack_usage.py
# report a clean pass over zero functions.
if [ -z "$(find "$SU_DIR" -maxdepth 1 -name '*.su' -print -quit)" ]; then
    echo "no .su stack-usage files were produced by $CC_BIN - it may not" >&2
    echo "support -fstack-usage. This gate exists to catch test fixtures" >&2
    echo "that would panic-loop the device; refusing to silently pass." >&2
    exit 1
fi

# shellcheck source=../../scripts/lib/python.sh
. "$TEST_DIR/../../scripts/lib/python.sh"
PYTHON=$(find_python) || exit 1
"$PYTHON" "$TEST_DIR/check_stack_usage.py" "$SU_DIR"

[ "$BUILD_ONLY" != 1 ] || exit 0

# The asset packs the suites read, one per root asset in the tree.
AUTANA_ASSET_DIR="$BUILD_DIR/assets"
export AUTANA_ASSET_DIR
"$PYTHON" "$TEST_DIR/../tools/r3d/build_pack.py" -o "$AUTANA_ASSET_DIR" "$MAIN_DIR" > /dev/null
# The test clip suite_anim_tracks.c holds to the Python sampler.
AUTANA_ANIM_PROBE="$BUILD_DIR/anim_probe.bin"
export AUTANA_ANIM_PROBE
"$PYTHON" "$TEST_DIR/../tools/tests/anim_probe.py" -o "$AUTANA_ANIM_PROBE"
# The scene suite_scene.c holds to the scene entry's Python writer.
AUTANA_SCENE_PROBE="$BUILD_DIR/scene_probe.bin"
export AUTANA_SCENE_PROBE
"$PYTHON" "$TEST_DIR/../tools/tests/scene_probe.py" -o "$AUTANA_SCENE_PROBE"

if [ "$SANITIZE" = 1 ] && [ "$(uname -s)" = Linux ]; then
    # Control ids are value addresses and must stay stable across frames, as on the device.
    ASAN_OPTIONS="${ASAN_OPTIONS:+$ASAN_OPTIONS:}detect_stack_use_after_return=0" "$OUT"
else
    "$OUT"
fi

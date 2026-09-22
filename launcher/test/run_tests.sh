#!/bin/sh
#
# Build and run the portable test suites on this machine.
#
#   ./test/run_tests.sh
#   CC=clang ./test/run_tests.sh
#   ./test/run_tests.sh --verbose
#
# This is the fast loop: it compiles for THIS machine, not the ESP32, and runs
# in well under a second. Red-green-refactor is only practical with instant
# feedback, and a build-and-flash cycle is about ninety seconds.
#
# It runs only the PORTABLE suites. The hardware ones need real framebuffer
# memory, DMA and I2C, so they live in the firmware and run at boot on the
# device - see main/selftest.c. The suite sources are shared, so what passes
# here is the same set of assertions the board makes.
#
# Default output is the verdict and test count. A passing run can emit
# thousands of characters; the full stream is in the printed log path.
#
# POSIX sh on purpose: works under Git Bash or MSYS on Windows, and natively
# on Linux and macOS.

set -eu

VERBOSE=${VERBOSE:-0}
while [ $# -gt 0 ]; do
    case "$1" in
        --verbose) VERBOSE=1 ;;
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
# Overridable so two runs cannot clobber each other: the build dir holds one
# host_tests binary, so concurrent runs (two terminals, a sweep script
# running beside a manual run) otherwise race to compile and execute the
# same file, and a result can end up attributed to a source state that never
# existed. Defaults to the old path, so nothing that does not set it changes.
BUILD_DIR="${TEST_BUILD_DIR:-$TEST_DIR/build}"

# --- find a compiler -------------------------------------------------------
# Sourced rather than defined here, so that report_reactions.sh (main/apps/
# sand/tools/) can find a compiler the same way without a hand-copied twin -
# see tools/find_cc.sh's own top comment.
# shellcheck source=../tools/find_cc.sh
. "$TEST_DIR/../tools/find_cc.sh"

if ! CC_BIN=$(find_cc); then
    echo "No C compiler found." >&2
    echo "  Windows: winget install BrechtSanders.WinLibs.POSIX.UCRT" >&2
    echo "  Debian:  sudo apt install build-essential" >&2
    echo "  macOS:   xcode-select --install" >&2
    exit 1
fi

# Warnings are errors: a host build catches mistakes the target build misses,
# and strictness costs nothing in tests.
CFLAGS="-std=c11 -Wall -Wextra -Werror -Wno-unused-parameter -g -O1"

# --- the device's heap, on this machine ------------------------------------
# Sourced the same way find_cc.sh is, one block above. The cap is a profile
# field rather than a literal here for the reason device_profile.sh's own
# header gives: it is a per-chip number, and a second board may join the
# test family. Selection is $DEVICE_PROFILE, default esp32s3.
# shellcheck source=../tools/device_profile.sh
. "$TEST_DIR/../tools/device_profile.sh"
device_profile_load "" "$TEST_DIR/../tools/device_profiles" || exit 1
HOST_HEAP_ARENA_BYTES=$(device_profile_require DP_FREE_HEAP_BYTES) || exit 1
HOST_HEAP_ARENA_PSRAM_BYTES=$(device_profile_require DP_PSRAM_BYTES) || exit 1
HOST_HEAP_ARENA_ALWAYSINTERNAL_BYTES=$(device_profile_require DP_SPIRAM_ALWAYSINTERNAL_BYTES) || exit 1
# One list for the test binary and --print-flags, so the complexity gate parses
# heap_arena.c with every define the real compile has.
HEAP_ARENA_DEFINES="-DHOST_HEAP_ARENA -DHOST_HEAP_ARENA_BYTES=$HOST_HEAP_ARENA_BYTES -DHOST_HEAP_ARENA_PSRAM_BYTES=$HOST_HEAP_ARENA_PSRAM_BYTES -DHOST_HEAP_ARENA_ALWAYSINTERNAL_BYTES=$HOST_HEAP_ARENA_ALWAYSINTERNAL_BYTES"

# The shell's own portable units and their suites. Hardware suites are absent
# by design - suite_gfx.c would not compile here, which is the point.
#
# gfx_dirty.h has no matching .c: it is header-only by necessity (see its
# own file comment - mark_band() has to stay inlinable into gfx.c), so
# suite_gfx_dirty.c pulls in its own copy of the whole thing just by
# including the header, with nothing extra to add to SOURCES for it.
SOURCES="
$TEST_DIR/host_main.c
$TEST_DIR/suites.c
$TEST_DIR/timing.c
$TEST_DIR/heap_arena.c
$TEST_DIR/suites/suite_touch_fsm.c
$TEST_DIR/suites/suite_gesture.c
$TEST_DIR/suites/suite_button_fsm.c
$TEST_DIR/suites/suite_rng.c
$TEST_DIR/suites/suite_fixed.c
$TEST_DIR/suites/suite_frame_cost.c
$TEST_DIR/suites/suite_tilt.c
$TEST_DIR/suites/suite_tune.c
$TEST_DIR/suites/suite_console.c
$TEST_DIR/suites/suite_tween.c
$TEST_DIR/suites/suite_spring_line.c
$TEST_DIR/suites/suite_boot_anim.c
$TEST_DIR/suites/suite_r3d_project.c
$TEST_DIR/suites/suite_r3d_camera.c
$TEST_DIR/suites/suite_gfx_dirty.c
$TEST_DIR/suites/suite_gfx_full_redraw.c
$TEST_DIR/suites/suite_gfx_present_guard.c
$TEST_DIR/suites/suite_gfx_fb_guard.c
$TEST_DIR/suites/suite_gfx_target.c
$TEST_DIR/suites/suite_gfx_mode.c
$TEST_DIR/suites/suite_gfx_band.c
$TEST_DIR/suites/suite_gfx_heal.c
$TEST_DIR/suites/suite_gfx_indexed.c
$TEST_DIR/suites/suite_gfx_palette.c
$TEST_DIR/suites/suite_small3dlib_scissor.c
$TEST_DIR/suites/suite_gfx_color.c
$TEST_DIR/suites/suite_gfx_glow.c
$TEST_DIR/suites/suite_gfx_font.c
$TEST_DIR/suites/suite_gfx_font_roles.c
$TEST_DIR/suites/suite_icons.c
$TEST_DIR/suites/suite_icons_system.c
$TEST_DIR/suites/suite_ui_style.c
$TEST_DIR/suites/suite_ui_transform.c
$TEST_DIR/suites/suite_ui_anchor.c
$TEST_DIR/suites/suite_control_center_layout.c
$TEST_DIR/suites/suite_ui_centered_rect.c
$TEST_DIR/suites/suite_ridge_curve.c
$TEST_DIR/suites/suite_ridge_motion.c
$TEST_DIR/suites/suite_ridge_pose.c
$TEST_DIR/suites/suite_ui_launcher.c
$TEST_DIR/suites/suite_ui_pointer.c
$TEST_DIR/suites/suite_ui_pointer_microui.c
$TEST_DIR/suites/suite_ui_scroll.c
$TEST_DIR/suites/suite_ui_slider.c
$TEST_DIR/suites/suite_display.c
$TEST_DIR/suites/suite_post_ui.c
$TEST_DIR/suites/suite_panel_clock.c
$TEST_DIR/suites/suite_screenshot.c
$TEST_DIR/suites/suite_app_registry.c
$TEST_DIR/suites/suite_build_id.c
$TEST_DIR/suites/suite_device_state.c
$TEST_DIR/suites/suite_job.c
$TEST_DIR/suites/suite_heap_caps.c
$MAIN_DIR/app_registry.c
$MAIN_DIR/input/touch_fsm.c
$MAIN_DIR/input/gesture.c
$MAIN_DIR/input/tilt.c
$MAIN_DIR/input/button_fsm.c
$MAIN_DIR/display/display.c
$MAIN_DIR/boot/post_layout.c
$MAIN_DIR/util/job.c
$MAIN_DIR/util/tune.c
$MAIN_DIR/console/console_verbs.c
$MAIN_DIR/display/panel_clock.c
$MAIN_DIR/ui/ui_build.c
$MAIN_DIR/ui/ui_launcher_draw.c
$MAIN_DIR/ui/ui_pointer.c
$MAIN_DIR/ui/ui_scroll.c
$MAIN_DIR/gfx/gfx_palette_standard.c
$MAIN_DIR/../tools/gfx_palette_gen.c
$TEST_DIR/../components/microui/src/microui.c
"

# App-owned sources, discovered rather than listed, so adding or deleting an
# app needs no change here.
#
# The convention: inside main/apps/<name>/, the file named app_*.c is the
# hardware-facing entry point - it talks to gfx, the IMU and the frame loop, so
# it cannot link on a host. A scene_*.c is the same kind of file: one of
# several hardware-facing renderers an app hosts behind its single app_*.c.
# Everything else in the folder is portable logic and is
# compiled in, along with any suite_*.c beside it.
#
# That split is not bureaucracy: it is what forces an app's logic to be
# separable from its wiring, which is the only reason a falling-sand automaton
# can be tested on a laptop at all.
#
# Recursive, matching main/CMakeLists.txt's own discovered_apps glob and its
# tools/ exclusion - a screen's drawing code lives one level deeper, in
# apps/<name>/ui/, so a one-level walk would silently drop it from this
# runner while the firmware kept building it. apps/<name>/tools/ - sweep
# scripts, report generators - is excluded the same way CMake excludes it:
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
# exact file list or flag set this script proves compilable - the clang-tidy
# complexity gate (tools/complexity_gate.py) builds its compile database
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
            -I "$MAIN_DIR" -I "$TEST_DIR" -I "$TEST_DIR/framework" \
            -I "$TEST_DIR/../components/microui/include" \
            -I "$TEST_DIR/../components/small3dlib/include" \
            -I "$TEST_DIR/../tools" -include "$TEST_DIR/timing.h" \
            $HEAP_ARENA_DEFINES
        exit 0
        ;;
esac

mkdir -p "$BUILD_DIR"
QUIET_LOG="$BUILD_DIR/run_tests.log"
if [ -z "${QUIET_INNER:-}" ]; then
    # shellcheck source=../../scripts/quiet.sh
    . "$TEST_DIR/../../scripts/quiet.sh"
    quiet_begin "$QUIET_LOG"
    quiet_run host-tests env QUIET_INNER=1 VERBOSE="$VERBOSE" sh "$0" "$@" || true
    QUIET_SUMMARY=$(grep -E '^[0-9]+ Tests [0-9]+ Failures [0-9]+ Ignored' "$QUIET_LOG" | tail -n 1)
    export QUIET_SUMMARY
    quiet_end run_tests || exit $?
    exit 0
fi

# The hardware-facing app_*.c files are excluded from SOURCES above because
# they cannot link here - which also meant nothing compiled them at all
# until a full device build. Compile-check them first, so a change that
# does not build is caught here rather than on the board.
"$TEST_DIR/check_app_sources.sh"

OUT="$BUILD_DIR/host_tests"

# Every suite calls RUN_TEST(func) directly; timing.h intercepts that macro
# (see its own comment) to log a wall-clock line per test without editing a
# single suite file - forced in ahead of everything else's own
# "#include unity.h" via -include below.
#
# unity.c itself must NOT see that -include: Unity's RUN_TEST is guarded by
# "#ifndef RUN_TEST", and if timing.h has already defined it by the time
# unity.c's own copy of that guard runs, Unity assumes a full replacement
# runner has been supplied (UNITY_SKIP_DEFAULT_RUNNER) and compiles
# UnityDefaultTestRun's body out entirely - the one function timing.c
# calls. So it is compiled alone, first, without -include.
UNITY_OBJ="$BUILD_DIR/unity.o"
# shellcheck disable=SC2086
"$CC_BIN" $CFLAGS -I "$MAIN_DIR" -I "$TEST_DIR" -I "$TEST_DIR/framework" \
    -c "$TEST_DIR/framework/unity.c" -o "$UNITY_OBJ"

# components/microui/include is on the path for ui_style.h's sake: it needs
# mu_Rect and mu_Color, and those are plain declarations in microui.h with no
# library behind them. microui.c itself IS linked now, for exactly one suite:
# suite_ui_pointer_microui.c drives the real widget code, because the event
# list ui_pointer.c emits can be perfectly correct and still produce a UI in
# which nothing is clickable - see that file's own comment. Every other
# suite here still needs only the declarations - see suite_ui_style.c on
# why a style's geometry was kept free of it.
#
# components/small3dlib/include is on the path for boot_anim.h's sake: its
# camera/space transforms are small3dlib's own S3L_Transform3D/S3L_Mat4 - see
# boot_anim.h's own top comment. Safe to include here alongside boot_anim.c's
# own translation unit (which this test binary does NOT compile - only
# suite_boot_anim.c, testing the pure math) because every small3dlib symbol
# is `static inline` - see small3dlib.h's own "PATCHED" comment for why that
# had to be true before this could work at all.
#
# -lm LAST, after the sources, because GNU ld resolves left to right and
# would otherwise have discarded libm before seeing who needed it. Only
# Linux actually needs it: the Windows toolchains this repo also builds on
# fold the math functions into libc, so a suite using atan2() or fabs()
# links clean locally and fails only in CI, which is exactly how it was
# found. Harmless where libm is already part of libc.
#
# --wrap routes the suite's own allocations into heap_arena.c's device-sized
# arena, so a fixture that asks for more than the board has fails HERE
# rather than after a flash. Only this runner defines HOST_HEAP_ARENA: any
# other build compiling the same timing.c gets every arena line preprocessed
# out, which is why the hooks had to be behind one macro rather than merely
# unused. Note that libc-internal allocations do not route
# through --wrap at all (a pointer from strdup() arrives at __wrap_free
# never having been seen by __wrap_malloc), which is why the arena forwards
# pointers it does not own instead of trusting every free().
# shellcheck disable=SC2086
"$CC_BIN" $CFLAGS -I "$MAIN_DIR" -I "$TEST_DIR" -I "$TEST_DIR/framework" \
    -I "$TEST_DIR/../components/microui/include" \
    -I "$TEST_DIR/../components/small3dlib/include" -I "$TEST_DIR/../tools" -include "$TEST_DIR/timing.h" \
    $HEAP_ARENA_DEFINES \
    $SOURCES "$UNITY_OBJ" -o "$OUT" \
    -Wl,--wrap=malloc -Wl,--wrap=calloc -Wl,--wrap=realloc -Wl,--wrap=free -lm

# --- static stack-frame gate ------------------------------------------------
# A separate, cheap compile pass over test-code translation units only, with
# -fstack-usage added - GCC/Clang then write one <name>.su file per object
# naming every function's own frame size, without executing anything. Kept
# out of the compile+link command above on purpose: -fstack-usage writes its
# .su file beside whatever -o path was given, and that command emits one
# binary from many files at once, so its .su files would scatter into the
# CWD rather than land somewhere this script can find them.
#
# Test code only (test/'s own drivers, test/suites/*.c, and each app's
# suite_*.c) - not the product logic those suites exercise. A huge frame in
# sand.c itself would be a real risk too, but it is not the risk that
# already panic-looped the board twice (see check_stack_usage.py's header),
# and widening this to product code is a separate decision. Derived from
# $SOURCES already assembled above, rather than a fresh glob, so this can
# only ever compile files already proven to build on a host: suite_gfx.c and
# suite_ui.c are device-only (real bsp/gfx headers, no host stub) and are
# already correctly absent from $SOURCES - globbing test/suites/*.c blindly
# would try to compile them here too and fail for a reason that has nothing
# to do with stack usage.
SU_DIR="$BUILD_DIR/su"
rm -rf "$SU_DIR"
mkdir -p "$SU_DIR"

SU_SOURCES=""
for f in $SOURCES; do
    case "$f" in
        "$TEST_DIR"/*) SU_SOURCES="$SU_SOURCES
$f" ;;
        */suite_*.c) SU_SOURCES="$SU_SOURCES
$f" ;;
    esac
done

# Compiled in parallel, in batches, because these are two dozen independent
# translation units and the cost is almost entirely per-process compiler
# launch overhead rather than -fstack-usage itself - serially this pass
# roughly doubled the wall time of a suite whose whole point is being fast
# enough to run constantly. Batched rather than all-at-once so this does not
# fork two dozen compilers on a small machine; `wait` without arguments is
# POSIX and waits for the whole batch.
#
# Each batch's exit statuses are collected into su_failed rather than
# checked with `set -e`, because a failing background job does not abort the
# script and an unnoticed compile failure here would mean a silently
# incomplete set of .su files - the exact "gate that checks nothing" this
# pass is trying not to be.
SU_BATCH=8
n=0
in_batch=0
su_pids=""
su_failed=0

su_wait_batch() {
    for pid in $su_pids; do
        wait "$pid" || su_failed=1
    done
    su_pids=""
    in_batch=0
}

for f in $SU_SOURCES; do
    n=$((n + 1))
    base=$(basename "$f" .c)
    # shellcheck disable=SC2086
    "$CC_BIN" $CFLAGS -I "$MAIN_DIR" -I "$TEST_DIR" -I "$TEST_DIR/framework" \
        -I "$TEST_DIR/../components/microui/include" \
        -I "$TEST_DIR/../components/small3dlib/include" -I "$TEST_DIR/../tools" -include "$TEST_DIR/timing.h" \
        -fstack-usage -c "$f" -o "$SU_DIR/$(printf '%02d' "$n")_$base.o" &
    su_pids="$su_pids $!"
    in_batch=$((in_batch + 1))
    [ "$in_batch" -lt "$SU_BATCH" ] || su_wait_batch
done
[ "$in_batch" -eq 0 ] || su_wait_batch

if [ "$su_failed" -ne 0 ]; then
    echo "the -fstack-usage pass failed to compile at least one test source;" >&2
    echo "its .su file is missing, so the stack gate would be checking an" >&2
    echo "incomplete set of functions. Refusing to continue." >&2
    exit 1
fi

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

# Same interpreter search as elsewhere in this tree: whatever python
# happens to be on PATH, python3 preferred.
PYTHON=$(command -v python3 || command -v python || true)
if [ -z "${PYTHON:-}" ]; then
    echo "no Python found to run check_stack_usage.py" >&2
    exit 1
fi
"$PYTHON" "$TEST_DIR/check_stack_usage.py" "$SU_DIR"

# MinGW appends .exe; elsewhere the plain name is produced.
[ -x "$OUT" ] || OUT="$OUT.exe"

"$OUT"

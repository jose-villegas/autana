#!/bin/sh
#
# The same stack-frame gate as run_tests.sh's, but against the frames the
# DEVICE's own compiler computes - the ground truth the host pass
# approximates.
#
#   ./check_stack_usage_device.sh
#
# Opt-in, not part of run_tests.sh: it needs the target's own cross
# toolchain, which a contributor running the host suite may not have. The
# host pass is the gate; this is how you check the gate is still telling
# the truth.
#
# It compiles every suite for the target with -fstack-usage, using the
# device profile's own ISA and codegen flags, and runs the same
# check_stack_usage.py over the result. No device, no flash, no idf.py -
# the suites are portable C, so the cross compiler alone is enough to get
# real target frames.
#
# WHY THIS IS WORTH RUNNING: the host gate assumes a frame that fits on x86
# is not much larger on the target. Measured 2026-09-19 with
# xtensa-esp32s3-elf-gcc over the 1,534 functions present in both builds:
# median 0.50x, but 67 are LARGER on Xtensa, worst 1.67x (576 -> 960 bytes,
# suite_gfx_font.c's glyph_run_boxes tests). Nothing crossed the ceiling here
# that the host had not also flagged, except code only a DEVICE_BUILD
# compiles. So the host is an estimate, not a bound - re-run this after a
# toolchain or -O-level change, and whenever a host frame nears the ceiling.
#
# POSIX sh, same portability reasoning as run_tests.sh.

set -eu

TEST_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
MAIN_DIR=$(CDPATH= cd -- "$TEST_DIR/../main" && pwd)
BUILD_DIR="${TEST_BUILD_DIR:-$TEST_DIR/build}/su-device"

# shellcheck source=../tools/device_profile.sh
. "$TEST_DIR/../tools/device_profile.sh"
. "$TEST_DIR/../tools/espressif.sh"
device_profile_load "" "$TEST_DIR/../tools/device_profiles" || exit 1

ARCH_FLAGS=$(device_profile_require DP_ARCH_FLAGS) || exit 1
CODEGEN_FLAGS=$(device_profile_require DP_CODEGEN_FLAGS) || exit 1
STD_FLAG=$(device_profile_require DP_STD_FLAG) || exit 1
PREFIX=$(device_profile_require DP_TOOLCHAIN_PREFIX) || exit 1

CC_BIN=$(command -v "$PREFIX-gcc" || true)
if [ -z "$CC_BIN" ]; then
    # Not on PATH under Git Bash unless the IDF environment was activated,
    # which costs ~90s and is not worth paying just to read frame sizes. The
    # tool directory is not named after the prefix: IDF ships every Xtensa
    # chip's driver in one xtensa-esp-elf install.
    for c in "$(espressif_tools_root)/tools"/*/*/*/bin/"$PREFIX-gcc.exe" \
             "$(espressif_tools_root)/tools"/*/*/*/bin/"$PREFIX-gcc"; do
        [ -x "$c" ] && CC_BIN="$c" && break
    done
fi
if [ -z "$CC_BIN" ]; then
    echo "no $PREFIX-gcc found - install the ESP-IDF toolchain, or run the" >&2
    echo "host gate alone via run_tests.sh." >&2
    exit 1
fi

rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"

# -ffreestanding because there is no target libc startup involved here: this
# only ever compiles, never links. DEVICE_BUILD matches what the device
# selftest compiles the suites with; a suite whose DEVICE_BUILD half needs
# ESP-IDF headers is measured as the host sees it instead, and one that
# needs them either way is outside the host gate too, so it is skipped aloud.
compile_suite() {
    # shellcheck disable=SC2086
    "$CC_BIN" $STD_FLAG $ARCH_FLAGS $CODEGEN_FLAGS -ffreestanding \
        -Wall -Wextra -Wno-unused-parameter -g \
        $1 -DCONFIG_LAUNCHER_DEVELOPMENT=1 \
        -I "$MAIN_DIR" -I "$TEST_DIR" -I "$TEST_DIR/framework" \
        -I "$TEST_DIR/stubs" \
        -I "$TEST_DIR/../components/microui/include" \
        -I "$TEST_DIR/../components/small3dlib/include" \
        -include "$TEST_DIR/timing.h" \
        -fstack-usage -c "$2" -o "$BUILD_DIR/$(basename "$2" .c).o" 2>/dev/null
}

for f in "$MAIN_DIR"/apps/*/tests/suite_*.c "$TEST_DIR"/suites/suite_*.c; do
    [ -e "$f" ] || continue
    if compile_suite -DDEVICE_BUILD "$f"; then
        continue
    elif compile_suite "" "$f"; then
        echo "portable half only (DEVICE_BUILD needs ESP-IDF headers): $(basename "$f")"
    else
        echo "skipped (needs ESP-IDF headers): $(basename "$f")"
    fi
done

if [ -z "$(find "$BUILD_DIR" -maxdepth 1 -name '*.su' -print -quit)" ]; then
    echo "no .su files produced by $CC_BIN - refusing to report a clean pass" >&2
    exit 1
fi

PYTHON=$(command -v python3 || command -v python || true)
if [ -z "${PYTHON:-}" ]; then
    echo "no Python found to run check_stack_usage.py" >&2
    exit 1
fi
echo "frames below are the TARGET's own, from $CC_BIN"
"$PYTHON" "$TEST_DIR/check_stack_usage.py" "$BUILD_DIR"

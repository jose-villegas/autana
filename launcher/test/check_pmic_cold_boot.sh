#!/bin/sh
#
# Build and run the bootloader's PMIC cold-boot hook against a simulated
# I2C bus, on this machine.
#
# pmic_cold_boot.c (launcher/bootloader_components/pmic_cold_boot/) is a
# bootloader-stage file: it links against ROM and register headers no host
# build provides and is never part of the app/test source lists run_tests.sh
# assembles. It is compiled here UNCHANGED, against the stand-in headers in
# stubs/bootloader/ and the bus/AXP2101 model in bootloader/pmic_i2c_bus.c,
# so the shipped bit-banging drives a simulated chip instead of real pins.
#
# A standalone binary rather than a suite_*.c: those share one process's
# Unity runner and link the ordinary main/ sources, neither of which applies
# to a bootloader-only file with its own tiny, disjoint header world.
#
# POSIX sh, like run_tests.sh: works under Git Bash or MSYS on Windows, and
# natively on Linux and macOS.

set -eu

HERE="$(cd "$(dirname "$0")" && pwd)"
BOOT_C_DIR="$HERE/../bootloader_components/pmic_cold_boot"
BOOT_TEST_DIR="$HERE/bootloader"
STUBS="$HERE/stubs/bootloader"
BUILD_DIR="${TEST_BUILD_DIR:-$HERE/build}"

# shellcheck source=../tools/build/find_cc.sh
. "$HERE/../tools/build/find_cc.sh"

if ! CC_BIN=$(find_cc); then
    echo "check_pmic_cold_boot: no C compiler found" >&2
    exit 1
fi

CFLAGS="-std=c11 -Wall -Wextra -Werror -g -O1"

mkdir -p "$BUILD_DIR"
OUT="$BUILD_DIR/pmic_cold_boot_host_test"

# shellcheck disable=SC2086
"$CC_BIN" $CFLAGS -I "$STUBS" -I "$BOOT_TEST_DIR" -I "$HERE/framework" \
    "$BOOT_C_DIR/pmic_cold_boot.c" \
    "$BOOT_TEST_DIR/pmic_i2c_bus.c" \
    "$BOOT_TEST_DIR/test_pmic_cold_boot.c" \
    "$HERE/framework/unity.c" \
    -o "$OUT"

[ -x "$OUT" ] || OUT="$OUT.exe"

"$OUT"

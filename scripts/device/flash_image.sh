#!/usr/bin/env bash
#
# Write a built firmware image to the board: the second half of
# `autana flash`, which device.py runs under the board's lock once
# launcher/tools/build/build_flash.sh has built the image.
#
# Usage:
#   scripts/device/flash_image.sh [--dev|--diag] [IDF_EXPORT]
#
#   --dev, --diag  write build.dev/ or build.diag/ instead of build/.
#   IDF_EXPORT     ESP-IDF's export script, as for build_flash.sh.
#
# This opens the board's serial port, which is why it lives in
# scripts/device/. It refuses unless AUTANA_DEVICE_LOCK_TOKEN is the live
# lock on AUTANA_BOARD (the board's USB serial number); device.py sets both
# after taking that lock. The token sits in plain text in the lock file, so
# this catches an accident, not a forger. The port is looked up only now: a
# board keeps its serial number but can come back from a reset on another COM.

set -euo pipefail

BUILD_DIR=build
IDF_EXPORT_ARG=""
while [ $# -gt 0 ]; do
    case "$1" in
        --dev)  BUILD_DIR=build.dev ;;
        --diag) BUILD_DIR=build.diag ;;
        -*)     echo "unknown option: $1" >&2; exit 2 ;;
        *)
            if [ -n "$IDF_EXPORT_ARG" ]; then
                echo "too many positional arguments" >&2
                exit 2
            fi
            IDF_EXPORT_ARG=$1
            ;;
    esac
    shift
done

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LAUNCHER_DIR="$(cd "$SCRIPT_DIR/../../launcher" && pwd)"

if [ -z "${AUTANA_DEVICE_LOCK_TOKEN:-}" ] || [ -z "${AUTANA_BOARD:-}" ]; then
    echo "ERROR: flashing needs the device lock on a named board." >&2
    echo "Run 'autana flash rel|dev|diag' instead." >&2
    exit 1
fi
if ! python "$SCRIPT_DIR/device_lock.py" --board "$AUTANA_BOARD" \
        check-token --token "$AUTANA_DEVICE_LOCK_TOKEN"; then
    echo "ERROR: device lock token is not active for board $AUTANA_BOARD" >&2
    echo "Run 'autana flash rel|dev|diag' instead." >&2
    exit 1
fi
COM_PORT=$(python "$SCRIPT_DIR/device.py" --board "$AUTANA_BOARD" resolve-port)

# shellcheck source=../../launcher/tools/build/idf.sh
. "$LAUNCHER_DIR/tools/build/idf.sh"
idf_init "$LAUNCHER_DIR" "$IDF_EXPORT_ARG" "$LAUNCHER_DIR/tools/build" || exit 2

echo "=== Flashing $BUILD_DIR to $COM_PORT ==="
idf -B "$BUILD_DIR" -p "$COM_PORT" flash
echo "=== Done - $BUILD_DIR is on the device ==="

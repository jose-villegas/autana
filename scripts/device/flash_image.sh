#!/usr/bin/env bash
#
# Write a snapshot of a built firmware image to the board: the second half of
# `autana flash`, which device.py runs under the board's lock once
# launcher/tools/build/build.sh has built the image and device.py has copied
# it out of the build directory.
#
# Usage:
#   scripts/device/flash_image.sh IMAGE_DIR [IDF_EXPORT]
#
#   IMAGE_DIR      a snapshot: flash_args, every file it lists, build_id.txt.
#   IDF_EXPORT     ESP-IDF's export script, as for build.sh.
#
# esptool writes the snapshot directly - never `idf.py flash`, whose target
# rebuilds first and writes whatever the build directory holds by then.
#
# This opens the board's serial port, which is why it lives in
# scripts/device/. It refuses unless _AUTANA_DEVICE_LOCK_TOKEN is the live
# lock on _AUTANA_BOARD (the board's USB serial number); device.py sets both
# after taking that lock. The token sits in plain text in the lock file, so
# this catches an accident, not a forger. The port is looked up only now: a
# board keeps its serial number but can come back from a reset on another COM.

set -euo pipefail

IMAGE_DIR=""
IDF_EXPORT_ARG=""
while [ $# -gt 0 ]; do
    case "$1" in
        -*) echo "unknown option: $1" >&2; exit 2 ;;
        *)
            if [ -z "$IMAGE_DIR" ]; then
                IMAGE_DIR=$1
            elif [ -z "$IDF_EXPORT_ARG" ]; then
                IDF_EXPORT_ARG=$1
            else
                echo "too many positional arguments" >&2
                exit 2
            fi
            ;;
    esac
    shift
done

if [ -z "$IMAGE_DIR" ] || [ ! -f "$IMAGE_DIR/flash_args" ] || [ ! -f "$IMAGE_DIR/build_id.txt" ]; then
    echo "ERROR: '$IMAGE_DIR' is not an image snapshot (flash_args, build_id.txt)." >&2
    echo "Run 'autana flash rel|dev|diag' instead." >&2
    exit 2
fi
IMAGE_DIR="$(cd "$IMAGE_DIR" && pwd)"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LAUNCHER_DIR="$(cd "$SCRIPT_DIR/../../launcher" && pwd)"
# shellcheck source=../lib/python.sh
. "$SCRIPT_DIR/../lib/python.sh"
PYTHON=$(find_python) || exit 2

if [ -z "${_AUTANA_DEVICE_LOCK_TOKEN:-}" ] || [ -z "${_AUTANA_BOARD:-}" ]; then
    echo "ERROR: flashing needs the device lock on a named board." >&2
    echo "Run 'autana flash rel|dev|diag' instead." >&2
    exit 1
fi
if ! "$PYTHON" "$SCRIPT_DIR/device_lock.py" --board "$_AUTANA_BOARD" \
        check-token --token "$_AUTANA_DEVICE_LOCK_TOKEN"; then
    echo "ERROR: device lock token is not active for board $_AUTANA_BOARD" >&2
    echo "Run 'autana flash rel|dev|diag' instead." >&2
    exit 1
fi
COM_PORT=$("$PYTHON" "$SCRIPT_DIR/device.py" --board "$_AUTANA_BOARD" resolve-port)

# shellcheck source=../../launcher/tools/build/idf.sh
. "$LAUNCHER_DIR/tools/build/idf.sh"
idf_init "$LAUNCHER_DIR" "$IDF_EXPORT_ARG" "$LAUNCHER_DIR/tools/build" || exit 2

BUILD_ID=$(tr -d '\r\n' < "$IMAGE_DIR/build_id.txt")
echo "=== Writing $BUILD_ID to $COM_PORT ==="
idf_in "$IMAGE_DIR" python -m esptool --chip esp32s3 -p "$COM_PORT" -b 460800 \
    --before default_reset --after hard_reset write_flash @flash_args
echo "=== Done - $BUILD_ID is on the device ==="

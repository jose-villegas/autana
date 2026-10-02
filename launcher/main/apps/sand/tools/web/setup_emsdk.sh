#!/bin/sh
#
# Clones and activates the Emscripten SDK into ./emsdk beside this script,
# for build_web.sh. Nothing outside that folder changes, so deleting it
# undoes the setup. Rerunning updates it instead.
#
#   launcher/main/apps/sand/tools/web/setup_emsdk.sh

set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
EMSDK_DIR="$SCRIPT_DIR/emsdk"

if [ -d "$EMSDK_DIR" ]; then
    git -C "$EMSDK_DIR" pull --ff-only
else
    git clone https://github.com/emscripten-core/emsdk.git "$EMSDK_DIR"
fi

cd "$EMSDK_DIR"
./emsdk install latest
./emsdk activate latest

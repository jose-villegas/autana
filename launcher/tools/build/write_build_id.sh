#!/bin/sh

set -eu

BUILD_DIR=$1
VARIANT=$2
ELF="$BUILD_DIR/launcher.elf"

if [ ! -f "$ELF" ]; then
    echo "no ELF at $ELF" >&2
    exit 1
fi

hash=$(sha256sum "$ELF" | awk '{print $1}' | tr -d '\\')
printf '%s-%s\n' "$(printf '%s' "$hash" | cut -c 1-12)" "$VARIANT" > "$BUILD_DIR/build_id.txt"

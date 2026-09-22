#!/bin/sh

set -eu

ELF=${1:-build/launcher.elf}
NM=${NM:-xtensa-esp32s3-elf-nm}

if [ ! -f "$ELF" ]; then
    echo "missing release ELF: $ELF" >&2
    exit 2
fi

symbols=$("$NM" --defined-only "$ELF" | awk '{print $NF}' |
    grep -E '(^|_)(suite|console|selftest)_' || true)
if [ -n "$symbols" ]; then
    echo "release image contains development or test symbols:" >&2
    echo "$symbols" >&2
    exit 1
fi

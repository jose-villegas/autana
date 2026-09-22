#!/bin/sh

set -eu

ELF=${1:-build/launcher.elf}
NM=${NM:-xtensa-esp32s3-elf-nm}

if [ ! -f "$ELF" ]; then
    echo "missing release ELF: $ELF" >&2
    exit 2
fi

if ! command -v "$NM" >/dev/null 2>&1; then
    for candidate in "${IDF_TOOLS_PATH:-$HOME/.espressif}"/tools/xtensa-esp-elf/*/xtensa-esp-elf/bin/xtensa-esp32s3-elf-nm*; do
        if [ -f "$candidate" ]; then
            NM=$candidate
            break
        fi
    done
fi

symbols=$("$NM" --defined-only "$ELF" | awk '{print $NF}' |
    grep -E '^(suite_|run_.*_suite$|console_(start$|emit_line$|reply_stdio$|take_unclaimed_line$|verb_)|selftest_)' || true)
if [ -n "$symbols" ]; then
    echo "release image contains development or test symbols:" >&2
    echo "$symbols" >&2
    exit 1
fi

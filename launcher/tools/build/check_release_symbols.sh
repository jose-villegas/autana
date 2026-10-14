#!/bin/sh

set -eu

ESPRESSIF_SCRIPT_DIR=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
. "$ESPRESSIF_SCRIPT_DIR/espressif.sh"

ELF=${1:-build/launcher.elf}
NM=${NM:-xtensa-esp32s3-elf-nm}

if [ ! -f "$ELF" ]; then
    echo "missing release ELF: $ELF" >&2
    exit 2
fi

if ! command -v "$NM" >/dev/null 2>&1; then
    for candidate in "$(espressif_tools_root)"/tools/xtensa-esp-elf/*/xtensa-esp-elf/bin/xtensa-esp32s3-elf-nm*; do
        if [ -f "$candidate" ]; then
            NM=$candidate
            break
        fi
    done
fi

if ! command -v "$NM" >/dev/null 2>&1; then
    echo "no nm for $ELF" >&2
    exit 2
fi

symbols_file=$(mktemp)
trap 'rm -f "$symbols_file"' EXIT
if ! "$NM" --defined-only "$ELF" >"$symbols_file"; then
    echo "nm failed for $ELF" >&2
    exit 2
fi

# shellcheck source=packages.sh
. "$ESPRESSIF_SCRIPT_DIR/packages.sh"

development_only_apps=
for app_dir in $(app_directories "$ESPRESSIF_SCRIPT_DIR/../.."); do
    marker="${app_dir}development_only.cmake"
    [ -f "$marker" ] || continue
    app=$(basename "${app_dir%/}")
    if ! grep -q "^APP_REGISTER(app_${app});" "${app_dir}app_${app}.c" 2>/dev/null; then
        echo "$marker: app_${app}.c does not register app_${app}" >&2
        exit 2
    fi
    development_only_apps="${development_only_apps}app_${app}\$|"
done

symbols=$(awk '{print $NF}' "$symbols_file" |
    grep -E "^(${development_only_apps}"'unity$|suite_|run_.*_suite$|console_(start$|emit_line$|reply_stdio$|take_unclaimed_line$|verb_)|selftest_)' || true)
if [ -n "$symbols" ]; then
    echo "release image contains development or test symbols:" >&2
    echo "$symbols" >&2
    exit 1
fi

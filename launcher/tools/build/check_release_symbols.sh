#!/bin/sh

set -eu

. "$(dirname "$0")/espressif.sh"

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

apps_dir="$(dirname "$0")/../../main/apps"
if [ ! -d "$apps_dir" ]; then
    echo "no apps directory at $apps_dir" >&2
    exit 2
fi

development_only_apps=
for marker in "$apps_dir"/*/development_only.cmake; do
    [ -f "$marker" ] || continue
    app=$(basename "$(dirname "$marker")")
    if ! grep -q "^APP_REGISTER(app_${app});" "$apps_dir/$app/app_${app}.c" 2>/dev/null; then
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

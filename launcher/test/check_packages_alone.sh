#!/bin/sh
#
# Compile every package's headers and sources with nothing on the include
# path but the package's own include/ root (tools/build/packages.sh).
#
# A package sits under every layer of the firmware, so it may include only
# itself, the C library and the build config. The style audit's PACKAGE-INCLUDE rule names a
# reach into the firmware's own folders; this catches the reaches a resolver
# cannot see: a stub, a vendored component, an ESP-IDF header. Each header is
# compiled on its own, so one that leans on whatever its includer happened to
# include first fails too. Syntax only: nothing is linked and nothing runs.
#
# Usage: check_packages_alone.sh [<launcher dir>]

set -eu

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
# The launcher to check: this one unless another is named, as a test's fixture is.
LAUNCHER_DIR=$(CDPATH= cd -- "${1:-$HERE/..}" && pwd)
# shellcheck source=../tools/build/find_cc.sh
. "$HERE/../tools/build/find_cc.sh"
# shellcheck source=../tools/build/packages.sh
. "$HERE/../tools/build/packages.sh"

if ! CC_BIN=$(find_cc); then
    echo "check_packages_alone: no C compiler found" >&2
    exit 1
fi

CFLAGS="-std=c11 -Wall -Wextra -Werror -fsyntax-only"

status=0
found=0
check() { # package-dir, file, compile command...
    found=$((found + 1))
    package=$(basename "$1")
    file=$2
    shift 2
    if ! "$@"; then
        echo "  FAIL $package: $file needs more than its package on the include path"
        status=1
    fi
}

UNIT_DIR=$(mktemp -d)
trap 'rm -rf "$UNIT_DIR"' EXIT
for include_dir in $(package_include_dirs "$LAUNCHER_DIR"); do
    package_dir=$(dirname "$include_dir")
    # A header goes in through a one-line unit, spelled as its includers spell it.
    for header in $(cd "$include_dir" && find . -name '*.h' | sed 's|^\./||' | sort); do
        printf '#include "%s"\n' "$header" >"$UNIT_DIR/unit.c"
        # shellcheck disable=SC2086
        check "$package_dir" "$header" "$CC_BIN" $CFLAGS -I "$include_dir" "$UNIT_DIR/unit.c"
    done
    for source in $(find "$package_dir/src" -name '*.c' 2>/dev/null | sort); do
        # shellcheck disable=SC2086
        check "$package_dir" "${source#"$package_dir"/}" "$CC_BIN" $CFLAGS -I "$include_dir" "$source"
    done
done

if [ "$found" -eq 0 ]; then
    echo "check_packages_alone: found no package to check" >&2
    exit 1
fi
[ "$status" -eq 0 ] && echo "check_packages_alone: $found package file(s) compile on their own"
exit "$status"

#!/bin/sh
#
# Compile-check the math tool probes (tools/math/*.c) on the host.
#
# The probes are C programs that doc_images runs to print the figures'
# numbers. Only the doc-image job built them, so a change to the maths API
# that the host tests passed left them broken until main's doc refresh went
# red. Syntax only: nothing is linked and nothing runs. Found by globbing, so
# a new probe needs no change here.
#
# Usage: check_tool_probes.sh

set -eu

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
LAUNCHER_DIR="$HERE/.."
# shellcheck source=../tools/build/find_cc.sh
. "$LAUNCHER_DIR/tools/build/find_cc.sh"
# shellcheck source=../tools/build/packages.sh
. "$LAUNCHER_DIR/tools/build/packages.sh"

if ! CC_BIN=$(find_cc); then
    echo "check_tool_probes: no C compiler found" >&2
    exit 1
fi

# The probes' own standard (see tools/math/geometry_figures.py), not the tests'.
CFLAGS="-std=gnu2x -Wall -Wextra -Werror -fsyntax-only"

status=0
found=0
for f in "$LAUNCHER_DIR"/tools/math/*.c; do
    [ -e "$f" ] || continue
    found=$((found + 1))
    # shellcheck disable=SC2046
    if "$CC_BIN" $CFLAGS -I "$LAUNCHER_DIR/main" $(package_includes "$LAUNCHER_DIR") "$f"; then
        echo "  ok   $(basename "$f")"
    else
        echo "  FAIL $(basename "$f")"
        status=1
    fi
done

if [ "$found" -eq 0 ]; then
    echo "check_tool_probes: found no tools/math/*.c" >&2
    exit 1
fi

exit "$status"

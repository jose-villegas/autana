#!/bin/sh
#
# Compile-check the doc-figure probes (tools/**/*_figures_probe.c) on the host.
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

# shellcheck source=../../scripts/lib/python.sh
. "$LAUNCHER_DIR/../scripts/lib/python.sh"
PYTHON=$(find_python) || { echo "check_tool_probes: no python found" >&2; exit 1; }

# The probes' own standard (tools/math/probe_standard.py), not the tests'.
PROBE_STANDARD=$("$PYTHON" "$LAUNCHER_DIR/tools/math/probe_standard.py")
CFLAGS="-std=$PROBE_STANDARD -Wall -Wextra -Werror -fsyntax-only"

status=0
found=0
for f in $(find "$LAUNCHER_DIR/tools" -name "*_figures_probe.c" | LC_ALL=C sort); do
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
    echo "check_tool_probes: found no tools/**/*_figures_probe.c" >&2
    exit 1
fi

exit "$status"

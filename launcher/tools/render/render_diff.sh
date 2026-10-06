#!/bin/sh
#
# Compare two frames of this panel pixel for pixel, a host render against a
# device capture, or any two of them.
#
#   ./launcher/tools/render/render_diff.sh <a> <b> [options]
#
# Every argument is passed straight to render_diff.py, which is where the
# options and the orientation and masking rules are documented. This only
# finds a Python and, under Git Bash, hands it paths that Windows can open.
#
# POSIX sh, like the rest of this directory.

set -eu

TOOLS_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

# shellcheck source=../../../scripts/lib/python.sh
. "$TOOLS_DIR/../../../scripts/lib/python.sh"
PYTHON=$(find_python) || exit 1

. "$TOOLS_DIR/../../../scripts/lib/native_path.sh"

remaining=$#
while [ "$remaining" -gt 0 ]; do
    arg=$1
    shift
    case "$arg" in
        /*) arg=$(to_native "$arg") || exit 1 ;;
    esac
    set -- "$@" "$arg"
    remaining=$((remaining - 1))
done

exec "$PYTHON" "$(to_native "$TOOLS_DIR/render_diff.py")" "$@"

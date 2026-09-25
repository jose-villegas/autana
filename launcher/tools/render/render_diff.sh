#!/bin/sh
#
# Compare two frames of this panel pixel for pixel - a host render against a
# device capture, or any two of them.
#
#   ./launcher/tools/render_diff.sh <a> <b> [options]
#
# Every argument is passed straight to render_diff.py, which is where the
# options and the orientation and masking rules are documented. This only
# finds a Python and, under Git Bash, hands it paths that Windows can open.
#
# POSIX sh, like the rest of this directory.

set -eu

TOOLS_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

if ! PYTHON=$(command -v python3 || command -v python); then
    echo "No Python found; render_diff.py needs one (standard library only)." >&2
    exit 1
fi

# Git Bash hands this script MSYS paths (/c/...), which the Windows python
# it finds cannot open. cygpath exists only there, which is also the only
# place the conversion is needed. An option or a number is left alone.
to_native() {
    if ! command -v cygpath > /dev/null 2>&1; then
        printf '%s' "$1"
        return
    fi
    case "$1" in
        /*) cygpath -w "$1" ;;
        *) printf '%s' "$1" ;;
    esac
}

remaining=$#
while [ "$remaining" -gt 0 ]; do
    arg=$1
    shift
    set -- "$@" "$(to_native "$arg")"
    remaining=$((remaining - 1))
done

exec "$PYTHON" "$(to_native "$TOOLS_DIR/render_diff.py")" "$@"

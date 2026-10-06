#!/bin/sh
# Requires Python 3 to resolve the pinned clang-format, then formats (or,
# with --check, verifies formatting of) the given C/C++ files using this
# repository's .clang-format rules (or the nearest one found by walking up
# from the file's directory).
#
# Usage:
#   check-format.sh <file.c> [file.h ...]         # format files in place
#   check-format.sh --check <file.c> [file.h ...] # verify only, no writes; exits 1 if not compliant
#   check-format.sh --which                       # print the clang-format this repo would use
#
# Pass files, never directories, and never the whole repository,
# scripts/gates/format-file-list.sh decides which files these rules apply to.

set -eu

GATE_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
. "$GATE_DIR/../lib/python.sh"
PYTHON=$(find_python) || exit 1
CLANG_FORMAT_RESULT=$("$PYTHON" "$GATE_DIR/../lib/pinned_tool.py" clang-format) || exit 1
CLANG_FORMAT_RESULT=$(printf '%s' "$CLANG_FORMAT_RESULT" | tr -d '\r')
CLANG_FORMAT_MAJOR=${CLANG_FORMAT_RESULT%%'
'*}
CLANG_FORMAT=${CLANG_FORMAT_RESULT#*'
'}

mode="format"
case "${1:-}" in
    --check)
        mode="check"
        shift
        ;;
    --which)
        mode="which"
        shift
        ;;
esac

if [ "$mode" = "which" ]; then
    command -v "$CLANG_FORMAT" || printf '%s\n' "$CLANG_FORMAT"
    exit 0
fi

if [ "$#" -eq 0 ]; then
    echo "Usage: $0 [--check] <file.c|file.h> [...]" >&2
    echo "       $0 --which" >&2
    exit 1
fi

if [ "$mode" = "check" ]; then
    "$CLANG_FORMAT" --dry-run --Werror "$@"
else
    "$CLANG_FORMAT" -i "$@"
    echo "Formatted with clang-format $CLANG_FORMAT_MAJOR: $*"
fi

#!/bin/sh
#
# Prints the first-party C and C++ files the formatting rules apply to,
# the single definition of that set, so the pre-commit hook and the CI gate
# cannot disagree about what is checked. docs/c-style/README.md is the prose;
# this is the file list.
#
# Usage:
#   scripts/gates/format-file-list.sh                  # every tracked first-party file
#   scripts/gates/format-file-list.sh <path> [...]     # only those paths that qualify
#   ... | scripts/gates/format-file-list.sh --stdin    # same, paths one per line
#
# The filtering forms print nothing (and still succeed) when no path
# qualifies, which is what lets a caller run it over an arbitrary commit's
# file list and act on the result.
#
# Two kinds of file are excluded, for the reason the style guide gives: they
# keep their upstream or generator-owned form.
#
#   Vendored, by path. A short list, because a vendored tree arrives whole
#   and is noticed.
#
#   Generated, by the "GENERATED FILE - do not edit." marker every
#   generator in this repo already writes into its first few lines. Deliberately not a path list: a
#   header that starts being generated tomorrow is excluded the day it
#   appears, not the day someone remembers to come back here. The cost is
#   that a generator which forgets the marker is silently held to the style
#   rules, which is the safe direction to fail.
#
# POSIX sh, like the other scripts here: development happens in Git Bash on
# Windows, and the hook has to run there too.

set -eu

# The files are the repository this is run in; the extensions are the
# source gates' own table, c_comments.SOURCE_EXTENSIONS.
GATE_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(git rev-parse --show-toplevel)
# shellcheck source=../lib/python.sh
. "$GATE_DIR/../lib/python.sh"
PYTHON=$(find_python) || exit 1
# Checked on its own: in a pipeline a failing Python would leave an empty
# table, and an empty table lists no file at all.
EXTENSIONS=$("$PYTHON" -c 'import sys; sys.path.insert(0, sys.argv[1]); from c_comments import SOURCE_EXTENSIONS; print(" ".join(SOURCE_EXTENSIONS))' "$GATE_DIR") || exit 1
EXTENSIONS=$(printf '%s' "$EXTENSIONS" | tr -d '\r')
if [ -z "$EXTENSIONS" ]; then
    echo "format-file-list.sh: c_comments.SOURCE_EXTENSIONS is empty" >&2
    exit 1
fi

has_source_extension() {
    for extension in $EXTENSIONS; do
        case "$1" in
            *"$extension") return 0 ;;
        esac
    done
    return 1
}

qualifies() {
    path="$1"

    has_source_extension "$path" || return 1

    # Vendored trees, checked in as-is. Patterns are spelled out in the case
    # itself rather than looped over a variable: an unquoted pattern in a
    # loop is subject to pathname expansion, which silently turns
    # "launcher/packages/testkit/unity/*" into the three paths it matches today and
    # lets everything else through. A case pattern is never expanded.
    case "$path" in
        launcher/components/esp32_s3_touch_amoled_1_8/* | \
            launcher/components/microui/* | \
            launcher/packages/testkit/unity/*)
            return 1
            ;;
    esac

    # A path can name a file that no longer exists (a deletion in the commit
    # being checked, or a stale argument). Nothing to format, so drop it
    # rather than failing; callers pass whole commit file lists.
    [ -f "$REPO_ROOT/$path" ] || return 1

    if head -5 "$REPO_ROOT/$path" | grep -qF 'GENERATED FILE - do not edit.'; then
        return 1
    fi

    return 0
}

emit_if_qualifies() {
    if qualifies "$1"; then
        printf '%s\n' "$1"
    fi
}

if [ "${1:-}" = "--stdin" ]; then
    while IFS= read -r path; do
        [ -n "$path" ] || continue
        emit_if_qualifies "$path"
    done
    exit 0
fi

if [ "$#" -gt 0 ]; then
    for path in "$@"; do
        emit_if_qualifies "$path"
    done
    exit 0
fi

set --
for extension in $EXTENSIONS; do
    set -- "$@" "*$extension"
done
git -C "$REPO_ROOT" ls-files -- "$@" | while IFS= read -r path; do
    emit_if_qualifies "$path"
done

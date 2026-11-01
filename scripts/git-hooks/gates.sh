#!/bin/sh
# Sourced by pre-commit and pre-push: sets ROOT to the repository being
# committed or pushed and GATES to the scripts/gates/ that judge it.
#
# core.hooksPath is one path shared by every worktree of a clone, so the
# gates normally come from the tree being judged, which may be on another
# branch than the one holding this file. A repository that holds this one
# as a submodule and points core.hooksPath into it gets the submodule's
# gates instead, whatever folders of its own it has.

ROOT=$(git rev-parse --show-toplevel) || exit 1
HOOK_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
# Asked of the hook's own tree: git exports GIT_DIR to a hook run in a
# worktree, which would answer for the repository being judged instead.
SUPERPROJECT=$(unset $(git rev-parse --local-env-vars)
               git -C "$HOOK_DIR" rev-parse --show-superproject-working-tree 2>/dev/null)
if [ "$SUPERPROJECT" = "$ROOT" ]; then
    GATES="$HOOK_DIR/../gates"
else
    GATES="$ROOT/scripts/gates"
fi

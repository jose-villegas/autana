#!/bin/sh
#
# Temporary detached checkouts shared by revision-comparison tools.

revision_worktree_setup() {
    REVISION_WORKTREE_REPO=$1
    REVISION_WORKTREE_DIR=$2
    REVISION_WORKTREE_CREATED="$REVISION_WORKTREE_DIR/worktrees.txt"
    : > "$REVISION_WORKTREE_CREATED"
}

revision_worktree_checkout() {
    _rwc_revision=$1
    _rwc_name=$2
    if [ -d "$_rwc_revision" ]; then
        (CDPATH= cd -- "$_rwc_revision" && pwd)
        return
    fi
    git -C "$REVISION_WORKTREE_REPO" rev-parse --verify --quiet "$_rwc_revision^{commit}" > /dev/null || {
        echo "not a revision or directory: $_rwc_revision" >&2
        return 1
    }
    _rwc_tree="$REVISION_WORKTREE_DIR/tree_$_rwc_name"
    git -C "$REVISION_WORKTREE_REPO" worktree add --detach "$_rwc_tree" "$_rwc_revision" > /dev/null
    git -C "$_rwc_tree" submodule update --init --recursive > /dev/null
    echo "$_rwc_tree" >> "$REVISION_WORKTREE_CREATED"
    printf '%s\n' "$_rwc_tree"
}

revision_worktree_cleanup() {
    while IFS= read -r _rwc_tree; do
        [ -n "$_rwc_tree" ] || continue
        git -C "$REVISION_WORKTREE_REPO" worktree remove --force "$_rwc_tree" > /dev/null 2>&1 || true
    done < "$REVISION_WORKTREE_CREATED"
}

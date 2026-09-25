#!/bin/sh
#
# Points this clone's git hooks at scripts/git-hooks/, which is tracked - so
# the hooks travel with the repository and an improvement to one reaches
# everybody who has run this once.
#
#   scripts/install-git-hooks.sh          # install
#   scripts/install-git-hooks.sh --status # what is configured right now
#   scripts/install-git-hooks.sh --remove # back to .git/hooks
#
# It sets core.hooksPath rather than copying files into .git/hooks, because a
# copy is a snapshot: it goes stale the moment the tracked hook changes, and
# nothing tells you. The tradeoff is that core.hooksPath replaces .git/hooks
# wholesale, so any hook you have there stops running - this script says so
# if it finds one.
#
# The hooks report format errors before commits and invalid branch names before
# pushes. CI enforces both checks on pull requests.

set -eu

REPO_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$REPO_ROOT"

HOOKS_DIR="scripts/git-hooks"

current=$(git config --local --get core.hooksPath || true)

case "${1:-}" in
    --status)
        if [ -n "$current" ]; then
            echo "core.hooksPath = $current"
            if [ "$current" != "$HOOKS_DIR" ]; then
                echo "  (not this repository's $HOOKS_DIR - pre-commit and pre-push hooks are NOT active)"
            else
                echo "  pre-commit checks formatting; pre-push checks target branch names."
            fi
        else
            echo "core.hooksPath is unset; git uses .git/hooks, so pre-commit and pre-push hooks are NOT active."
            echo "Run scripts/install-git-hooks.sh to turn it on."
        fi
        exit 0
        ;;
    --remove)
        if [ -z "$current" ]; then
            echo "core.hooksPath was not set; nothing to remove."
        else
            git config --local --unset core.hooksPath
            echo "Removed core.hooksPath. Git uses .git/hooks again."
        fi
        exit 0
        ;;
    "") ;;
    *)
        echo "Usage: $0 [--status|--remove]" >&2
        exit 1
        ;;
esac

# A hook of our own is fine to replace; one somebody wrote is not something
# to disable silently.
existing=""
if [ -d .git/hooks ]; then
    for hook in .git/hooks/*; do
        [ -f "$hook" ] || continue
        case "$hook" in
            *.sample) continue ;;
        esac
        existing="$existing  $hook
"
    done
fi

if [ -n "$existing" ]; then
    echo "NOTE: .git/hooks already contains hooks:" >&2
    printf '%s' "$existing" >&2
    echo "core.hooksPath replaces that directory entirely, so those will stop running." >&2
    echo "Move anything you want to keep into $HOOKS_DIR (tracked), or run --remove to undo." >&2
    echo "" >&2
fi

git config --local core.hooksPath "$HOOKS_DIR"

# Git needs the hook executable. The bit is tracked, but a checkout on a
# filesystem that drops it (or a Windows clone with core.fileMode off) would
# leave a hook that silently never runs.
chmod +x "$HOOKS_DIR"/* 2>/dev/null || true

echo "Installed: core.hooksPath -> $HOOKS_DIR"
echo ""
echo "Active hooks:"
for hook in "$HOOKS_DIR"/*; do
    [ -f "$hook" ] || continue
    echo "  $(basename "$hook")"
done
echo ""
echo "The pre-commit hook checks that staged C and header files are formatted"
echo "(scripts/gates/check-format-staged.sh). It needs clang-format 19 - see"
echo "docs/C-Style-Guide.md. The pre-push hook checks target branch names with"
echo "scripts/gates/check-branch-name.sh."
echo "Undo with: scripts/install-git-hooks.sh --remove"

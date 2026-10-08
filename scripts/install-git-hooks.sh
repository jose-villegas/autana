#!/bin/sh
#
# Points this clone's git hooks at scripts/git-hooks/, which is tracked, so
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
# wholesale, so any hook you have there stops running: this script says so
# if it finds one.
#
# The hooks check staged formatting, executable bits and text rules before
# commits, then branch names and sanitized host tests before C/header pushes.

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
                echo "  pre-commit checks staged formatting, exec bits and text rules."
                echo "  pre-push checks branch names and runs sanitized host tests for C/header changes."
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
echo "Pre-commit checks staged formatting, executable bits and CI's file-scoped text rules."
echo "It needs clang-format 19 and Python with scripts/gates/requirements.txt installed."
echo "See docs/C-Style-Guide.md. Pre-push checks branch names and runs host tests"
echo "with --sanitize when a pushed range changes C or headers. See docs/Testing-Guide.md."
echo "Skip a hook with git commit --no-verify or git push --no-verify."
echo "Undo with: scripts/install-git-hooks.sh --remove"

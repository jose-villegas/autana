#!/usr/bin/env bash
#
# Puts this checkout's tools/ folder on the user's PATH, so `autana` runs
# from any terminal, and proves it did. See docs/tools/Autana-CLI.md.
#
#   scripts/add-tools-to-path.sh            # add, then verify
#   scripts/add-tools-to-path.sh --check    # verify only, change nothing
#
# The folder is found from this script's own place, never from the working
# directory, so it is right whichever checkout it is run from - and it should
# be run from the PRIMARY checkout, the one that stays put; a worktree is
# deleted sooner or later and would leave a dead PATH entry. What a command
# ACTS on is still the worktree you are standing in, so one PATH entry serves
# every worktree.
#
# AUTANA_RECORDS, if a .dev checkout sits beside this one, points device.py's
# session records at .dev/records/device, where this project keeps and tracks
# them. Without it they land in this checkout's own gitignored .records/.
#
# Windows: the persistent USER Path in the registry, no admin rights, which
# cmd, PowerShell and Git Bash all inherit. Elsewhere: one line in ~/.profile.
# Either way it is a no-op when the folder is already there, and a terminal
# that is already open keeps its old PATH - open a new one.
#
# Verification does not trust this shell's PATH: it builds the PATH a NEW
# terminal would get and resolves `autana` in that, then runs it.
set -u

check_only=0
[ "${1:-}" = "--check" ] && check_only=1

repo=$(cd "$(dirname "$0")/.." && pwd -P)
root="$repo/tools"
[ -f "$root/autana" ] || { echo "no autana launcher in $root" >&2; exit 1; }

# Empty when there is no .dev beside this checkout - device.py treats that
# as no setting at all and uses its own default.
records=""
[ -d "$repo/.dev/records/device" ] && records="$repo/.dev/records/device"

on_windows() {
    local win_root
    win_root=$(cygpath -w "$root")
    local win_records=""
    [ -n "$records" ] && win_records=$(cygpath -w "$records")
    AUTANA_TOOLS_ROOT="$win_root" AUTANA_CHECK_ONLY="$check_only" AUTANA_RECORDS_DIR="$win_records" \
        powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$(cygpath -w "$repo/scripts/lib/add-tools-to-path.ps1")"
}

on_posix() {
    local line="export PATH=\"$root:\$PATH\"  # autana CLI"
    [ -n "$records" ] && line="$line
export AUTANA_RECORDS=\"$records\"  # autana device records"
    local profile="$HOME/.profile"
    if grep -qsF "$root:" "$profile"; then
        echo "already on PATH: $root ($profile)"
    elif [ "$check_only" = 1 ]; then
        echo "NOT on PATH: $root" >&2
        return 1
    else
        printf '\n%s\n' "$line" >> "$profile"
        echo "added to $profile: $root"
    fi
    local found
    found=$(bash -lc 'command -v autana') || { echo "a new login shell does not find autana" >&2; return 1; }
    echo "a new shell finds: $found"
    bash -lc 'autana --help' > /dev/null || { echo "autana found but did not run" >&2; return 1; }
    echo "and it runs."
}

case "$(uname -s)" in
    MINGW* | MSYS* | CYGWIN*) on_windows ;;
    *) on_posix ;;
esac

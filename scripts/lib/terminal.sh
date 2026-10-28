#!/bin/sh
#
# pause_if_own_window holds a failed run's window open until Enter, but only
# when that window was opened for this run and closes when it exits: a .sh
# double-clicked in Explorer (Git for Windows runs it in mintty) or a
# terminal emulator told to run the script. Such a script is the session
# leader of its own terminal. A script started by a shell, another script or
# a pipe is not, and returns at once whatever its stdin is.
#
# A Windows console never pauses. From bash, a hidden console (a detached
# Start-Process), a visible one and a PowerShell caller's look the same:
# session leader, parent outside MSYS, /dev/consN.
#
#   . scripts/lib/terminal.sh
#   [ "$status" -eq 0 ] || pause_if_own_window

owns_window() {
    [ -t 0 ] || return 1
    if [ -r "/proc/$$/ctty" ]; then
        # MSYS and Cygwin: mintty gives its command a pty.
        [ "$(cat "/proc/$$/sid")" = "$$" ] || return 1
        case "$(cat "/proc/$$/ctty")" in /dev/pty*) return 0 ;; esac
        return 1
    fi
    [ -r "/proc/$$/stat" ] || return 1
    _terminal_stat=$(cat "/proc/$$/stat")
    # Fields after "(comm)": state ppid pgrp session tty_nr; tty_nr 0 is no
    # controlling terminal, as after setsid with an inherited tty.
    # shellcheck disable=SC2086
    set -- ${_terminal_stat##*) }
    [ "$4" = "$$" ] && [ "$5" != 0 ]
}

pause_if_own_window() {
    owns_window || return 0
    printf 'Press Enter to close...'
    read -r _terminal_dismissed || true
}

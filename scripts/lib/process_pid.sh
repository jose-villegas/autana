#!/bin/sh
# Map a POSIX shell pid to the host pid used by Python's liveness checks.
host_pid() {
    if [ -r "/proc/$1/winpid" ]; then cat "/proc/$1/winpid"
    else printf '%s\n' "$1"
    fi
}

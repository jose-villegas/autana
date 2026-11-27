#!/bin/sh
# Map a POSIX shell pid to the host pid used by Python's liveness checks,
# in HOST_PID; builtins only, so it starts no process.
host_pid() {
    if [ -r "/proc/$1/winpid" ]; then read -r HOST_PID < "/proc/$1/winpid"
    else HOST_PID=$1
    fi
}

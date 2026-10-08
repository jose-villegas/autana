#!/bin/sh
# A failed child command retains its status and names its complete invocation.
run() {
    if "$@"; then
        return 0
    else
        code=$?
        printf '%s: failed (exit %s):' "$0" "$code" >&2
        printf ' %s' "$@" >&2
        printf '\n' >&2
        return "$code"
    fi
}

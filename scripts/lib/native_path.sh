#!/bin/sh
# Native paths for tools launched from Git Bash; other shells pass through.
to_native() {
    if command -v cygpath >/dev/null 2>&1; then
        if [ "$#" -eq 0 ]; then
            cygpath -m -f -
        else
            cygpath -m "$1"
        fi
    elif [ "$#" -eq 0 ]; then
        cat
    else
        printf '%s\n' "$1"
    fi
}

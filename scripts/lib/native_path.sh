#!/bin/sh
# Shell adapter to the shared native-path converter; its owner is NATIVE_PATH_LIB.
. "$(dirname -- "$NATIVE_PATH_LIB")/python.sh"
NATIVE_PATH_PYTHON=$(find_python) || return 1

to_native() {
    "$NATIVE_PATH_PYTHON" "$NATIVE_PATH_LIB" "$@"
}

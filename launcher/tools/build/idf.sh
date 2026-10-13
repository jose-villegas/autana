#!/bin/sh
# ESP-IDF refuses Git Bash's MSYSTEM. The batch shim deletes it and runs
# export.bat while keeping sequencing and error handling in POSIX sh.
# Measured alternatives:
#   env -u MSYSTEM python     -> MSYS re-injects MSYSTEM
#   MSYSTEM= python           -> guard tests presence, not value
#   cmd /c "set MSYSTEM=&& ?"  -> nested quotes corrupt argv
#   cmd /c shim.bat ?         -> argv and semicolons survive
# Linux sources export.sh directly.

_IDF_DIR=""
_IDF_EXPORT=""
_IDF_SHIM=""

# Resolve substitutions here: failures inside caller arguments do not fail
# the call. _IDF_ state avoids blanking a caller's environment variables.
# idf_init <launcher-dir> [tools-dir]; $0 names the caller, so callers outside
# this tools directory must supply it explicitly.
idf_init() {
    _IDF_DIR="$1"
    _IDF_TOOLS="${2:-$(cd "$(dirname "$0")" && pwd)}"
    . "$_IDF_TOOLS/../../../scripts/lib/python.sh"
    _IDF_PYTHON=$(find_python) || return 1
    _IDF_ENV=$("$_IDF_PYTHON" "$_IDF_TOOLS/espressif.py" env "$_IDF_DIR/..") || return 1
    eval "$_IDF_ENV"
    export IDF_PATH IDF_TOOLS_PATH
    _IDF_EXPORT="$(idf_default_export)" || return 1
    _IDF_SHIM="$_IDF_TOOLS/idf_shim.bat"
    "$_IDF_PYTHON" "$_IDF_TOOLS/espressif.py" note "$_IDF_DIR/.." || return 1
}

# The resolver selects the pin unless a process override selects a checkout.
idf_default_export() {
    if idf_needs_shim; then
        _IDF_DEFAULT="$IDF_PATH/export.bat"
    else
        _IDF_DEFAULT="$IDF_PATH/export.sh"
    fi
    if [ ! -f "$_IDF_DEFAULT" ]; then
        echo "no ESP-IDF export script at $_IDF_DEFAULT" >&2
        return 1
    fi
    if idf_needs_shim; then
        cygpath -w "$_IDF_DEFAULT"
    else
        echo "$_IDF_DEFAULT"
    fi
}

idf_needs_shim() {
    [ -n "${MSYSTEM:-}" ]
}

idf() {
    idf_in "$_IDF_DIR" idf.py "$@"
}

idf_in() {
    _IDF_IN_DIR=$1
    shift
    if idf_needs_shim; then
        # cmd needs cygpath paths, but ordinary argv must not be pre-quoted:
        # inline quotes reach the batch shim literally and corrupt arguments.
        IDF_SHIM_DIR="$(cygpath -w "$_IDF_IN_DIR")" \
        IDF_SHIM_EXPORT="$_IDF_EXPORT" \
        cmd //c "$(cygpath -w "$_IDF_SHIM")" "$@"
    else
        ( cd "$_IDF_IN_DIR" && . "$_IDF_EXPORT" >/dev/null 2>&1 && "$@" )
    fi
}

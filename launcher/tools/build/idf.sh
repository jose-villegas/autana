#!/bin/sh
# Run ESP-IDF commands in a process-scoped environment. Windows calls use
# a batch shim to remove MSYSTEM before running ESP-IDF's export script.

_IDF_DIR=""
_IDF_EXPORT=""
_IDF_SHIM=""

idf_init() {
    _IDF_DIR="$1"
    _IDF_TOOLS="${3:-$(cd "$(dirname "$0")" && pwd)}"
    . "$_IDF_TOOLS/../../../scripts/lib/python.sh"
    _IDF_PYTHON=$(find_python) || return 1
    if [ -n "${2:-}" ]; then
        IDF_PATH=$(dirname "$2")
        export IDF_PATH
    fi
    _IDF_ENV=$("$_IDF_PYTHON" "$_IDF_TOOLS/espressif.py" env "$_IDF_DIR/..") || return 1
    eval "$_IDF_ENV"
    export IDF_PATH IDF_TOOLS_PATH
    _IDF_EXPORT="${2:-$(idf_default_export)}" || return 1
    _IDF_SHIM="$_IDF_TOOLS/idf_shim.bat"
    if [ "${_AUTANA_IDF_NOTED:-}" != 1 ]; then
        "$_IDF_PYTHON" "$_IDF_TOOLS/espressif.py" note "$_IDF_DIR/.." || return 1
    fi
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
        IDF_SHIM_DIR="$(cygpath -w "$_IDF_IN_DIR")" \
        IDF_SHIM_EXPORT="$_IDF_EXPORT" \
        cmd //c "$(cygpath -w "$_IDF_SHIM")" "$@"
    else
        ( cd "$_IDF_IN_DIR" && . "$_IDF_EXPORT" >/dev/null 2>&1 && "$@" )
    fi
}

#!/bin/sh
#
# Run idf.py, or any command that needs ESP-IDF's environment, from a POSIX
# shell on any platform. Source this and call idf() or idf_in().
#
#   . "$(dirname "$0")/idf.sh"
#   idf_init "/path/to/launcher" || exit $?
#   idf -B build build                           || exit $?
#   idf_in <dir> python -m esptool ...           || exit $?
#
# Which ESP-IDF: espressif.py decides (the checkout at the commit in
# launcher/idf-pin.json, unless IDF_PATH is set for the command); idf_init
# asks it once and passes that checkout and its tools to every call, for that
# process only.
#
# WHY THE SHIM
#
# ESP-IDF cannot be driven from Git Bash: idf_tools.py refuses outright when
# MSYSTEM is set, and Git Bash always sets it. cmd's `set VAR=` deletes a
# variable where bash can only blank it, and ESP-IDF ships export.bat, so a
# short .bat shim (idf_shim.bat, beside this file) is enough, and every
# conditional, loop and error path stays here in sh. Measured before being
# written, because each step had a plausible-looking alternative that does
# not work:
#
#   env -u MSYSTEM python     -> MSYSTEM=MINGW64  (MSYS re-injects it)
#   MSYSTEM= python           -> present but empty; the guard tests presence
#   cmd /c "set MSYSTEM=&& ..." -> works, but nested quotes reach argv literally
#   cmd /c shim.bat ...       -> works, argv intact, semicolons survive
#
# Linux keeps sourcing export.sh in its own shell; Windows selects Python
# before export.bat because activation itself needs the paired interpreter.

_IDF_DIR=""
_IDF_EXPORT=""
_IDF_SHIM=""

# idf_init <launcher-dir> [tools-dir]
#
# The checkout is resolved in here rather than by the caller: a substitution
# that fails inside an argument leaves the command's status at 0, so
# `idf_init "$dir" "$(...)"` would carry on with no ESP-IDF at all.
#
# State is kept in _IDF_-prefixed variables: sourcing a file that declares a
# plain name would silently blank a caller's variable of that name.
#
# tools-dir defaults to the calling script's own directory, which is right
# for everything in launcher/tools/. Pass it explicitly from anywhere else;
# `$0` is the CALLER's path, and POSIX sh has no portable way for a sourced
# file to find itself.
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

# The export script of the checkout idf_init selected; on Windows it goes to
# cmd, so it is spelled as a Windows path.
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
        IDF_SHIM_PYTHON_DIR="$IDF_SHIM_PYTHON_DIR" \
        cmd //c "$(cygpath -w "$_IDF_SHIM")" "$@"
    else
        ( cd "$_IDF_IN_DIR" && . "$_IDF_EXPORT" >/dev/null 2>&1 && "$@" )
    fi
}

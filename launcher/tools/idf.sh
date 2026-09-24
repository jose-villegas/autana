#!/bin/sh
#
# Run idf.py from a POSIX shell, on any platform. Source this and call idf().
#
#   . "$(dirname "$0")/idf.sh"
#   idf_init "/path/to/launcher" || exit $?
#   idf -B build build               || exit $?
#   idf -B build -p <PORT> flash     || exit $?
#
# WHY THIS EXISTS
#
# ESP-IDF cannot be driven from Git Bash: idf_tools.py refuses outright when
# MSYSTEM is set, and Git Bash always sets it. The tooling here used to work
# around that by embedding PowerShell - each script carried its own block of
# `Remove-Item Env:\MSYSTEM`, `Set-Location`, `if ($LASTEXITCODE -ne 0)`, and
# the actual sequencing logic went with it. Seven such blocks across three
# scripts, each a small PowerShell program in a file that was otherwise sh.
#
# It turns out none of that is necessary. cmd's `set VAR=` deletes a variable
# where bash can only blank it, and ESP-IDF ships export.bat alongside
# export.ps1 - so a nine-line .bat shim (idf_shim.bat, beside this file) is
# enough, and every conditional, loop and error path stays here in sh.
#
# Measured before being written, because each step had a plausible-looking
# alternative that does not work:
#
#   env -u MSYSTEM python    -> MSYSTEM=MINGW64  (MSYS re-injects it)
#   MSYSTEM= python          -> present but empty; the guard tests presence
#   cmd /c "set MSYSTEM=&& …" -> works, but nested quotes reach argv literally
#   cmd /c shim.bat …        -> works, argv intact, semicolons survive
#
# On Linux and macOS none of this applies: idf.py is called directly.

_IDF_DIR=""
_IDF_EXPORT=""
_IDF_SHIM=""

# idf_init <launcher-dir> [export.bat-or-export.sh] [tools-dir]
#
# An empty export means idf_default_export(), resolved in here rather than by
# the caller: `idf_init "$dir" "$(idf_default_export)"` loses the failure,
# because a substitution that fails inside an argument leaves the command's
# status at 0, and idf_init would carry on with no export script at all.
#
# State is kept in _IDF_-prefixed variables. Sourcing a file that declares
# plain IDF_EXPORT would silently blank a caller's variable of that name
# before it ever reached idf_init - which is exactly what happened, and cost
# a debugging round: the shim was invoked with an empty export path and cmd
# failed on `call ""` with nothing useful to say.
#
# tools-dir defaults to the calling script's own directory, which is right
# for everything in launcher/tools/. Pass it explicitly from anywhere else -
# `$0` is the CALLER's path, not this file's, and POSIX sh has no portable
# way for a sourced file to find itself.
idf_init() {
    _IDF_DIR="$1"
    _IDF_EXPORT="${2:-$(idf_default_export)}" || return 1
    _IDF_SHIM="${3:-$(cd "$(dirname "$0")" && pwd)}/idf_shim.bat"
}

# The export script of the ESP-IDF at $IDF_PATH, which ESP-IDF's installers
# set. On Windows it goes to cmd, so it is spelled as a Windows path whichever
# way IDF_PATH arrived. $HOME/esp/esp-idf is Espressif's documented checkout.
idf_default_export() {
    if idf_needs_shim; then
        _IDF_DEFAULT="${IDF_PATH:-}/export.bat"
    else
        _IDF_DEFAULT="${IDF_PATH:-$HOME/esp/esp-idf}/export.sh"
    fi
    if [ ! -f "$_IDF_DEFAULT" ]; then
        echo "no ESP-IDF found: set IDF_PATH to your ESP-IDF checkout, or pass its export script" >&2
        return 1
    fi
    if idf_needs_shim; then
        cygpath -w "$_IDF_DEFAULT"
    else
        echo "$_IDF_DEFAULT"
    fi
}

# Whether this shell needs the Windows shim at all.
idf_needs_shim() {
    [ -n "${MSYSTEM:-}" ]
}

# Run one idf.py invocation. Returns its exit status.
idf() {
    if idf_needs_shim; then
        # cygpath so cmd gets Windows paths; the arguments themselves are
        # passed through as ordinary argv and must NOT be pre-quoted - see
        # idf_shim.bat for why inline quoting corrupts them.
        IDF_SHIM_DIR="$(cygpath -w "$_IDF_DIR")" \
        IDF_SHIM_EXPORT="$_IDF_EXPORT" \
        cmd //c "$(cygpath -w "$_IDF_SHIM")" idf.py "$@"
    else
        # POSIX host: source the environment once per call, same as the shim
        # does, so the two paths behave identically.
        ( cd "$_IDF_DIR" && . "$_IDF_EXPORT" >/dev/null 2>&1 && idf.py "$@" )
    fi
}

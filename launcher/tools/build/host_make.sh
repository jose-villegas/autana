#!/bin/sh
#
# Helpers for incremental host builds: find make, find ccache, pick a job
# count. Source this file; nothing runs on source.
#
#   . "$TOOLS_DIR/host_make.sh"
#   MAKE_BIN=$(find_make) || exit 1
#   CCACHE_BIN=$(find_ccache) || CCACHE_BIN=""
#   JOBS=$(host_jobs)
#
# ccache is optional everywhere: without it a build is only slower.
# AUTANA_CCACHE=0 turns it off even when installed.

# GNU make is `make` on Linux and `mingw32-make` on Windows, where WinLibs
# (the documented compiler) ships it beside gcc.
find_make() {
    for m in make gmake mingw32-make; do
        if command -v "$m" >/dev/null 2>&1; then echo "$m"; return 0; fi
    done
    winlibs="${LOCALAPPDATA:-}/Microsoft/WinGet/Packages/BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe/mingw64/bin/mingw32-make.exe"
    if [ -n "${LOCALAPPDATA:-}" ] && [ -x "$winlibs" ]; then echo "$winlibs"; return 0; fi
    echo "No GNU make found (Debian: sudo apt install build-essential; Windows: WinLibs ships mingw32-make)." >&2
    return 1
}

# ccache on PATH, else the copy ESP-IDF installs under its tools directory.
find_ccache() {
    [ "${AUTANA_CCACHE:-1}" = 0 ] && return 1
    if command -v ccache >/dev/null 2>&1; then echo ccache; return 0; fi
    for root in "${IDF_TOOLS_PATH:-}" "${HOME:-}/.espressif" "${USERPROFILE:-}/.espressif"; do
        [ -n "$root" ] || continue
        for c in "$root"/tools/ccache/*/ccache* "$root"/tools/ccache/*/*/ccache*; do
            case "$c" in
                *.exe | */ccache) [ -x "$c" ] && { echo "$c"; return 0; } ;;
            esac
        done
    done
    return 1
}

# Half the logical CPUs, at most 8: a full-width compile fan-out once starved
# the whole machine of memory. HOST_JOBS overrides.
host_jobs() {
    if [ -n "${HOST_JOBS:-}" ]; then echo "$HOST_JOBS"; return 0; fi
    n=$(nproc 2>/dev/null || echo "${NUMBER_OF_PROCESSORS:-2}")
    n=$((n / 2))
    [ "$n" -ge 1 ] || n=1
    [ "$n" -le 8 ] || n=8
    echo "$n"
}

#!/bin/sh
#
# find_python [module ...] prefers the minor pinned in .python-version,
# then the first interpreter on PATH meeting scripts/lib/python-min-version
# and importing every named module. Fallback tries python3, python, py, then
# versioned python3.N names newest first in each directory.
#
#   PYTHON=$(find_python PIL) || exit 2

# POSIX sh has no source-file variable; its entry script belongs to the same
# tree. Bash also supports sourcing this library from an unrelated directory.
_python_source=${BASH_SOURCE:-$0}
case "$_python_source" in
    /*|[A-Za-z]:/*) _python_root=${_python_source%/*} ;;
    */*) _python_root=$PWD/${_python_source%/*} ;;
    *) _python_root="" ;;
esac
while [ -n "$_python_root" ] && [ ! -r "$_python_root/scripts/lib/python-min-version" ]; do
    case "$_python_root" in /|[A-Za-z]:) break ;; esac
    _python_root=${_python_root%/*}
    _python_root=${_python_root:-/}
done
PYTHON_MIN_VERSION=""
if [ -n "$_python_root" ] && [ -r "$_python_root/scripts/lib/python-min-version" ]; then
    IFS= read -r PYTHON_MIN_VERSION < "$_python_root/scripts/lib/python-min-version"
fi
if [ -z "$PYTHON_MIN_VERSION" ]; then
    echo "cannot locate scripts/lib/python-min-version" >&2
    return 1
fi

PYTHON_PINNED_MINOR=""
if [ -r "$_python_root/.python-version" ]; then
    IFS= read -r PYTHON_PINNED_MINOR < "$_python_root/.python-version"
    PYTHON_PINNED_MINOR=${PYTHON_PINNED_MINOR%.*}
fi

python_meets_minimum() {
    _pm_candidate=$1
    shift
    "$_pm_candidate" -c 'import sys
minimum = tuple(map(int, sys.argv[1].split(".")))
if sys.version_info[:2] < minimum:
    sys.exit(1)
for name in sys.argv[2:]:
    __import__(name)' "$PYTHON_MIN_VERSION" "$@" >/dev/null 2>&1
}

find_python() {
    if [ -n "$PYTHON_PINNED_MINOR" ]; then
        _fp_pinned=$(command -v "python$PYTHON_PINNED_MINOR") || _fp_pinned=""
        if [ -n "$_fp_pinned" ] && python_meets_minimum "$_fp_pinned" "$@"; then
            printf '%s\n' "$_fp_pinned"
            return 0
        fi
    fi
    _fp_rest="$PATH:"
    while [ -n "$_fp_rest" ]; do
        _fp_dir=${_fp_rest%%:*}
        _fp_rest=${_fp_rest#*:}
        [ -n "$_fp_dir" ] || continue
        _fp_names="python3 python py"
        _fp_versioned=""
        for _fp_path in "$_fp_dir"/python3.[0-9] "$_fp_dir"/python3.[0-9][0-9]; do
            [ -x "$_fp_path" ] && [ ! -d "$_fp_path" ] || continue
            _fp_versioned="${_fp_path##*/} $_fp_versioned"
        done
        if [ -n "$_fp_versioned" ]; then
            _fp_names="$_fp_names $_fp_versioned"
        fi
        for _fp_name in $_fp_names; do
            _fp_candidate="$_fp_dir/$_fp_name"
            [ -x "$_fp_candidate" ] && [ ! -d "$_fp_candidate" ] || continue
            if python_meets_minimum "$_fp_candidate" "$@"; then
                printf '%s\n' "$_fp_candidate"
                return 0
            fi
        done
    done
    echo "no Python >= $PYTHON_MIN_VERSION${*:+ with $*} found on PATH; install Python >= $PYTHON_MIN_VERSION (Windows: py launcher or ESP-IDF venv)." >&2
    return 1
}

# find_r3d_python <repo-root> prints the interpreter that has the r3d tools'
# requirements: the venv under launcher/tools/r3d/.cache when there is one,
# else the first find_python with mitsuba and scipy.
#
#   R3D_PYTHON=$(find_r3d_python "$ROOT") || exit 2

find_r3d_python() {
    for _fr_candidate in Scripts/python bin/python; do
        if [ -x "$1/launcher/tools/r3d/.cache/venv/$_fr_candidate" ]; then
            printf '%s\n' "$1/launcher/tools/r3d/.cache/venv/$_fr_candidate"
            return 0
        fi
    done
    find_python mitsuba scipy
}

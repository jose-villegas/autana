#!/bin/sh
#
# find_python [module ...] prints the first interpreter on PATH that is a
# real Python 3 and imports every named module, and fails with a message
# when there is none. Debian and Ubuntu ship only `python3`, Windows only
# `python` (its `python3` may be a Store stub that does not run), and a
# Windows venv only `python`, so the search walks PATH in order and tries
# python3, python and py in each directory: whichever environment comes
# first on PATH, an activated venv included, wins regardless of its names.
#
#   PYTHON=$(find_python PIL) || exit 2

find_python() {
    _fp_rest="$PATH:"
    while [ -n "$_fp_rest" ]; do
        _fp_dir=${_fp_rest%%:*}
        _fp_rest=${_fp_rest#*:}
        [ -n "$_fp_dir" ] || continue
        for _fp_name in python3 python py; do
            _fp_candidate="$_fp_dir/$_fp_name"
            [ -x "$_fp_candidate" ] && [ ! -d "$_fp_candidate" ] || continue
            if "$_fp_candidate" -c 'import sys
if sys.version_info < (3,):
    sys.exit(1)
for name in sys.argv[1:]:
    __import__(name)' "$@" >/dev/null 2>&1; then
                printf '%s
' "$_fp_candidate"
                return 0
            fi
        done
    done
    echo "no Python 3${*:+ with $*} found on PATH (tried python3, python, py)." >&2
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

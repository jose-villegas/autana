#!/bin/sh
#
# find_python [module ...] prints the first interpreter that is a real
# Python 3 and imports every named module, and fails with a message when
# there is none. Debian and Ubuntu ship only `python3`, Windows only
# `python` (its `python3` is a Store stub that does not run), so no script
# may hard-code either name.
#
#   PYTHON=$(find_python PIL) || exit 2

find_python() {
    for _fp_candidate in python3 python py; do
        command -v "$_fp_candidate" >/dev/null 2>&1 || continue
        if "$_fp_candidate" -c 'import sys
if sys.version_info < (3,):
    sys.exit(1)
for name in sys.argv[1:]:
    __import__(name)' "$@" >/dev/null 2>&1; then
            printf '%s\n' "$_fp_candidate"
            return 0
        fi
    done
    echo "no Python 3${*:+ with $*} found on PATH (tried python3, python, py)." >&2
    return 1
}

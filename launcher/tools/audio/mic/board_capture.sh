#!/usr/bin/env bash
# One microphone capture job on one board, inside the caller's board lock: flash this tree's
# development image, then run capture scripts with the build id recorded before and after.
#
#   board_capture.sh <board> <capture.py> [args...] [--then <capture.py> [args...]]...
#
# Each capture script gets --board and --autana (this tree's autana); its own args follow.
set -euo pipefail
[ $# -ge 2 ] || { echo "usage: board_capture.sh <board> <capture.py> [args...] [--then <capture.py> [args...]]..." >&2; exit 2; }
root=$(cd "$(dirname "$0")/../../../.." && pwd)
. "$root/scripts/lib/python.sh"
python=$(find_python)
autana="$root/tools/autana"
board=$1
shift
script_path() {
    # A board job hands paths inside the shipped tree over as absolute paths.
    case $1 in /* | [A-Za-z]:*) printf '%s\n' "$1" ;; *) printf '%s\n' "$root/$1" ;; esac
}
run_capture() {
    local script
    script=$(script_path "$1")
    shift
    "$python" "$script" --board "$board" --autana "$autana" "$@"
}
# Every capture imports its tools before the board is flashed, so a host missing them stops here.
next_is_script=1
for arg in "$@"; do
    if [ "$next_is_script" = 1 ] && ! "$python" "$(script_path "$arg")" --help >/dev/null; then
        echo "board_capture: $arg cannot start on this host; install launcher/tools/audio/mic/requirements.txt" >&2
        exit 1
    fi
    next_is_script=0
    [ "$arg" = --then ] && next_is_script=1
done
"$autana" --board "$board" status
"$autana" --board "$board" flash dev
"$autana" --board "$board" buildid
capture=()
for arg in "$@"; do
    if [ "$arg" = --then ]; then
        run_capture "${capture[@]}"
        capture=()
    else
        capture+=("$arg")
    fi
done
run_capture "${capture[@]}"
"$autana" --board "$board" buildid
"$autana" --board "$board" status

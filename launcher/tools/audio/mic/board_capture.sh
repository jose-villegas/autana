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
run_capture() {
    local script=$1
    shift
    # A board job hands paths inside the shipped tree over as absolute paths.
    case $script in /* | [A-Za-z]:*) ;; *) script=$root/$script ;; esac
    "$python" "$script" --board "$board" --autana "$autana" "$@"
}
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

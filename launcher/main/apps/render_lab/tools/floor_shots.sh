#!/bin/sh
#
# Board screenshots of render lab scenes held at poses along their camera
# path, each at the chosen render size and at the dynamic-resolution
# ladder's floor, to judge what the floor costs in picture.
#
#   floor_shots.sh [--flash] --out <dir>/shots.txt <scene>@<ms> [<scene>@<ms> ...]
#
# <scene> is a key from `autana render scenes` with a camera path (Sponza
# or Bistro); <ms> is the time along it. Each pose writes
# <scene>-<ms>-chosen.png and <scene>-<ms>-floor.png beside the --out file,
# which lists them and the build id the board ran. --flash builds and
# flashes the development image first. Run it under one `autana lock run`.

set -eu

FLASH=0
if [ "${1:-}" = --flash ]; then
    FLASH=1
    shift
fi
[ "${1:-}" = --out ] && [ $# -ge 3 ] || { echo "usage: floor_shots.sh [--flash] --out <dir>/shots.txt <scene>@<ms>..." >&2; exit 2; }
LIST=$2
shift 2
DIR=$(dirname -- "$LIST")
mkdir -p "$DIR"
: > "$LIST"

# A budget of 1 ms is under every step's price, so the predictor sits on the
# floor; 0 is the fixed chosen scale.
FLOOR_BUDGET_MS=1
CHOSEN_BUDGET_MS=0

# A switch lands at the next frame and a large scene loads for seconds.
SWITCH_TRIES=30

showing() {
    autana render scenes | grep -q "scene=$1 .*current=1"
}

[ "$FLASH" = 0 ] || autana flash dev --quiet
autana buildid >> "$LIST"
autana open "Render Lab" > /dev/null
for pose in "$@"; do
    scene=${pose%@*}
    ms=${pose#*@}
    autana render scene "$scene" > /dev/null
    tries=0
    until showing "$scene"; do
        tries=$((tries + 1))
        [ "$tries" -lt "$SWITCH_TRIES" ] || { echo "floor_shots: $scene did not show" >&2; exit 1; }
    done
    autana render pose "$ms" > /dev/null
    for size in chosen floor; do
        if [ "$size" = floor ]; then budget=$FLOOR_BUDGET_MS; else budget=$CHOSEN_BUDGET_MS; fi
        autana tune render_lab.budget "$budget" > /dev/null
        shot="$DIR/$scene-$ms-$size.png"
        autana screenshot -o "$shot" > /dev/null
        echo "$shot $size budget=$budget" >> "$LIST"
    done
    autana render pose play > /dev/null
done
autana tune render_lab.budget "$CHOSEN_BUDGET_MS" > /dev/null
cat "$LIST"

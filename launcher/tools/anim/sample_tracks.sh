#!/bin/sh
#
# Build and run sample_tracks over one baked animation: what every track
# holds every N milliseconds, or a poses file for a camera node.
#
# Usage:
#   launcher/tools/anim/sample_tracks.sh --tracks SOURCE.c:NAME [--every MS] [--until MS] [--clamp]
#   launcher/tools/anim/sample_tracks.sh --tracks SOURCE.c:NAME [--every MS] --poses NODE W H TAN NEAR
#
#   --tracks SOURCE.c:NAME  the C file bake_tracks.py wrote and the --name it was given
#
# The poses go to report_triangle_sizes.sh on a pipe, so they are always
# the animation's own:
#   sample_tracks.sh --tracks A.c:NAME --poses camera 184 224 0.62 6 | report_triangle_sizes.sh --mesh M.c:SYM -

set -eu

# anim -> tools -> launcher.
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
LAUNCHER_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)
MAIN_DIR="$LAUNCHER_DIR/main"

usage() {
    echo "usage: $0 --tracks SOURCE.c:NAME [options]" >&2
    exit 2
}

[ "${1:-}" = --tracks ] && [ $# -ge 2 ] || usage
case "$2" in
*.c:?*) ;;
*) usage ;;
esac
tracks_source=${2%:*}
tracks_name=${2##*:}
shift 2
[ -f "$tracks_source" ] || { echo "no tracks source $tracks_source" >&2; exit 2; }

# shellcheck source=../build/find_cc.sh
. "$LAUNCHER_DIR/tools/build/find_cc.sh"
if ! CC_BIN=$(find_cc); then
    echo "No C compiler found." >&2
    exit 1
fi

BUILD_DIR="$SCRIPT_DIR/build"
mkdir -p "$BUILD_DIR"
OUT_BIN="$BUILD_DIR/sample_tracks"
"$CC_BIN" -std=c11 -Wall -Wextra -Werror -Wno-unused-parameter -O2 \
    -I "$MAIN_DIR" -I "$(dirname -- "$tracks_source")" \
    -DANIM_TRACKS="${tracks_name}_tracks" -DANIM_TRACK_COUNT="${tracks_name}_track_count" \
    "$SCRIPT_DIR/sample_tracks_main.c" "$tracks_source" "$MAIN_DIR/anim/anim_track.c" \
    -lm -o "$OUT_BIN"
[ -x "$OUT_BIN" ] || OUT_BIN="$OUT_BIN.exe"
"$OUT_BIN" "$@"

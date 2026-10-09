#!/bin/sh
#
# The render harness's frame watch, checked against a fixture scene: work on
# every frame must fail the render with the FRAME_WATCH line naming its
# kind, the same work once must pass, and stdout must come back whole.
#
#   ./launcher/tools/render/tests/check_frame_watch.sh
#
# Run by tools/render/render_all_scenes.sh. The fixture is built exactly as
# a scene is (render_scene_build) and then run by hand, since a failing
# render is the expected result for half of these cases.
#
# POSIX sh, like the rest of this directory.

set -eu
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

scene_name=frame_watch_fixture
scene_sources="
main/services/tune.c
tools/render/tests/frame_watch_fixture.c
"
scene_renders=""
scene_pin=0

# shellcheck source=../render_scene.sh
. "$SCRIPT_DIR/../render_scene.sh"
render_scene_build "$@"

failed=0
log="$scene_out_dir/check.log"
out="$scene_out_dir/check.stdout"
image="$scene_out_dir/check.bmp"

fail() {
    cat "$log" >&2
    echo "FAIL frame watch: $1" >&2
    failed=1
}

# expect <exit status> <verdict kind or ""> <what> <fixture arguments...>
expect() {
    want=$1
    verdict=$2
    what=$3
    shift 3
    status=0
    "$_rs_bin" "$@" -o "$image" 2> "$log" > "$out" || status=$?
    if [ "$status" != "$want" ]; then
        fail "$what - exited $status, expected $want"
    elif [ -n "$verdict" ] && ! grep -q "^FRAME_WATCH $verdict in " "$log"; then
        fail "$what - no 'FRAME_WATCH $verdict' line"
    elif ! grep -q "^FRAME_WATCH judged " "$log"; then
        fail "$what - no FRAME_WATCH judged line"
    else
        echo "ok frame watch: $what"
    fi
}

expect 1 alloc "malloc every frame fails" --malloc every
expect 0 "" "malloc on one frame passes" --malloc once
expect 0 "" "three malloc sites taking turns are not merged" --malloc turns
expect 1 alloc "calloc every frame fails" --calloc every
expect 0 "" "calloc on one frame passes" --calloc once
expect 1 alloc "realloc every frame fails" --realloc every
expect 0 "" "realloc on one frame passes" --realloc once
expect 1 free "free every frame fails" --free every
expect 0 "" "free on one frame passes" --free once
expect 1 console "a print every frame fails" --print every
expect 0 "" "a print on one frame passes" --print once
expect 0 "" "a frame that does neither passes"

# The print on one frame again: stdout given back in order, capture gone.
"$_rs_bin" --print once -o "$image" 2> "$log" > "$out"
want_out="fixture setup
fixture last frame
fixture frame 24"
if [ "$(tr -d '\r' < "$out")" != "$want_out" ]; then
    fail "stdout came back as: $(cat "$out")"
elif [ -e "$image.console" ]; then
    fail "the stdout capture $image.console was left behind"
else
    echo "ok frame watch: stdout comes back in order and its capture is removed"
fi

exit "$failed"

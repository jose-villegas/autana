#!/bin/sh
#
# The render harness's frame watch, checked against a fixture scene: work on
# every frame must fail the render, the same work on one frame must pass.
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
main/gfx/gfx.c
main/util/tune.c
tools/render/tests/frame_watch_fixture.c
"
scene_renders=""
scene_pin=0

# shellcheck source=../render_scene.sh
. "$SCRIPT_DIR/../render_scene.sh"
render_scene_build "$@"

failed=0

# expect <exit status> <what> <fixture arguments...>
expect() {
    want=$1
    what=$2
    shift 2
    log="$scene_out_dir/check.log"
    status=0
    "$_rs_bin" "$@" -o "$scene_out_dir/check.bmp" 2> "$log" > /dev/null || status=$?
    if [ "$status" = "$want" ]; then
        echo "ok frame watch: $what"
    else
        cat "$log" >&2
        echo "FAIL frame watch: $what - exited $status, expected $want" >&2
        failed=1
    fi
}

expect 1 "an allocation every frame fails" --alloc every
expect 0 "an allocation on one frame passes" --alloc once
expect 1 "a print every frame fails" --print every
expect 0 "a print on one frame passes" --print once
expect 0 "a frame that does neither passes"

exit "$failed"

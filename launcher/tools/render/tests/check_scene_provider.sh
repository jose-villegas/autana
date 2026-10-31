#!/bin/sh
# Check provider path hooks with an authored fixture supplied by the caller.
set -eu
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
scene_name=scene_provider_fixture
scene_provider=
scene_sources="main/core/job.c tools/render/tests/scene_provider_fixture.c"
scene_renderer=1
scene_renders=""
. "$SCRIPT_DIR/../render_scene.sh"
render_scene_build "$@"
"$_rs_bin" --scene room -o "$scene_out_dir/hooks.bmp"

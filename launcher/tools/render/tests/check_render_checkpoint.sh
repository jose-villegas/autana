#!/bin/sh
# Check saved frame history with the caller's animated scene asset.
set -eu
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
scene_name=render_checkpoint_fixture
scene_provider=
scene_sources="tools/render/tests/render_checkpoint_fixture.c"
scene_target=render_host
scene_renders=""
. "$SCRIPT_DIR/../render_scene.sh"
render_scene_build "$@"
"$_rs_bin" --scene room --present double -o "$scene_out_dir/checkpoint.bmp"

#!/bin/sh
# The declared scene checks camera-path drawing without pinning float coverage.
set -eu
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
scene_name=scene_viewer
scene_provider=scene_provider
scene_target=scene_provider
scene_sources=
scene_assets=demo/sponza
# The render draws the scene's showcase.
. "$SCRIPT_DIR/../../../../scripts/lib/python.sh"
. "$SCRIPT_DIR/../../../../scripts/lib/native_path.sh"
showcase=$("$(find_python)" "$(to_native "$SCRIPT_DIR/../showcase.py")" "$(to_native "$SCRIPT_DIR/../../../$scene_assets/sponza.scene.toml")")
scene_renders="landscape|--scene sponza --object $showcase --quarter 1 --frames 2|448x368|nopin"
. "$SCRIPT_DIR/../render_scene.sh"
render_scene_run "$@"

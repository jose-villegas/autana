#!/bin/sh
# Builds and runs the recorded-frame provider through the shared host harness.
set -eu
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
scene_name=stream_render
scene_provider=stream_provider
scene_target=stream_provider
scene_sources=
scene_renders=
. "$SCRIPT_DIR/render_scene.sh"
for argument in "$@"; do
    case "$argument" in
        --build-only|--deps) render_scene_run "$@"; exit $? ;;
    esac
done
render_scene_build --build-only
exec "$_rs_bin" "$@"

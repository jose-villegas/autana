#!/bin/sh
# The declared scene checks camera-path drawing without pinning float coverage.
set -eu
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
scene_name=scene_viewer
scene_sources="
main/services/tune.c
main/core/job.c
main/render/raster.c
main/render/raster_show.c
main/render/raster_motion.c
main/render/raster_meshlets.c
main/render/r3d_pipeline.c
main/render/upscale.c
main/render/resolution/resolution.c
main/render/context/render_context.c
main/anim/anim_track.c
main/anim/anim_tracks.c
main/render/r3d_span.c
main/render/r3d_scene.c
main/scene/scene.c
main/scene/scene_asset.c
main/scene/scene_draw.c
main/scene/scene_shell.c
main/render/r3d_lit_mesh.c
tools/render/scene_viewer.c
"
scene_assets=demo/sponza
scene_renders="landscape|--scene sponza --object atrium --quarter 1 --frames 2|448x368|nopin"
. "$SCRIPT_DIR/../render_scene.sh"
render_scene_run "$@"

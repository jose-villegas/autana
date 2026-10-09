#!/bin/sh
#
# Render this app's scenes on a host, with no board and no flash cycle.
#
#   ./launcher/main/apps/render_lab/tools/render_lab_render_host.sh [-o <dir>] [--update-baseline]
#
# Each declared render starts on one scene (--scene) via
# render_lab_start_scene_key and steps 16 ms frames, full-framebuffer
# layout. Landscape is the shipping orientation, so it leads; a panel-native
# render is the shape tools/render/render_diff.sh compares a device capture against.
#
# Everything beyond the declarations below - finding a compiler, building,
# checking each image against its declared size and pin, converting to PNG -
# is launcher/tools/render/render_scene.sh.

set -eu
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

scene_name=render_lab

scene_sources="
main/app/app_arena.c
main/app/app_registry.c
main/util/runtime/tune.c
main/util/runtime/job.c
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
main/util/motion/orbit_motion.c
main/input/gesture.c
main/input/orbit_motion_touch.c
main/ui/ui.c
main/ui/ui_bridge.c
main/ui/ui_build.c
main/ui/ui_canvas_marks.c
main/ui/ui_widgets.c
main/ui/ui_pointer.c
main/ui/ui_snap.c
main/ui/ui_scroll.c
components/microui/src/microui.c
main/apps/render_lab/tools/render_lab_render_host.c
$(CDPATH= cd -- "$SCRIPT_DIR/../../../../" &&
    find main/apps/render_lab \( -type d \( -name tools -o -name tests \) -prune \) -o \
        \( -type f -name '*.c' ! -name 'suite_*.c' -print \) | LC_ALL=C sort)
"
scene_defines="-DCONFIG_LAUNCHER_DEVELOPMENT=0"

# The lit-mesh scene's views (--view shaded|depth|tiles|motion|meshlets) are raster_show()
# over the same frame, so each is unpinned for the reason the shaded one is: its
# camera path is float, so which pixels a triangle reaches is not identical on
# every compiler. The tests in tools/tests compare the views with each other.
#
# The cube and wire scenes with the HUD hidden are pinned: triangle and line
# coverage is integer, and the floats appear only in projection, truncated to
# whole pixels, built without fast-math or FMA. Everything carrying the
# HUD is not - its fps readout is a double printed with "%.1f" - and neither
# are the Cornell scenes, which are float throughout.
scene_renders="
gouraud-landscape|--quarter 1 --no-hud --scene gouraud|448x368
gouraud-landscape-panel|--quarter 1 --panel --no-hud --scene gouraud|368x448
gouraud-portrait|--quarter 0 --no-hud --scene gouraud|368x448
plane-landscape|--quarter 1 --no-hud --scene plane|448x368
plane-portrait|--quarter 0 --no-hud --scene plane|368x448
sphere-landscape|--quarter 1 --no-hud --scene sphere|448x368
sphere-portrait|--quarter 0 --no-hud --scene sphere|368x448
wirecube-landscape|--quarter 1 --no-hud --scene cube|448x368
capsule-landscape|--quarter 1 --no-hud --scene capsule|448x368
gouraud-landscape-hud|--quarter 1 --scene gouraud|448x368|nopin
gouraud-landscape-panel-hud|--quarter 1 --panel --scene gouraud|368x448|nopin
gouraud-portrait-hud|--quarter 0 --scene gouraud|368x448|nopin
plane-landscape-hud|--quarter 1 --scene plane|448x368|nopin
plane-portrait-hud|--quarter 0 --scene plane|368x448|nopin
sphere-landscape-hud|--quarter 1 --scene sphere|448x368|nopin
sphere-portrait-hud|--quarter 0 --scene sphere|368x448|nopin
wirecube-landscape-hud|--quarter 1 --scene cube|448x368|nopin
capsule-landscape-hud|--quarter 1 --scene capsule|448x368|nopin
plane-title-fading|--quarter 1 --scene plane --frames 110|448x368|nopin
plane-title-gone|--quarter 1 --scene plane --frames 140|448x368|nopin
cornell-landscape|--quarter 1 --scene cornell --frames 40|448x368|nopin
cornell-portrait|--quarter 0 --scene cornell --frames 40|368x448|nopin
cornell-pass1|--quarter 1 --scene cornell --frames 2|448x368|nopin
cornell-pass2|--quarter 1 --scene cornell --frames 5|448x368|nopin
cornell-pass3|--quarter 1 --scene cornell --frames 9|448x368|nopin
cornell-pt-landscape|--quarter 1 --scene cornell-pt --frames 60|448x368|nopin
cornell-pt-portrait|--quarter 0 --scene cornell-pt --frames 60|368x448|nopin
cornell-pt-seed|--quarter 1 --scene cornell-pt --frames 2|448x368|nopin
cornell-pt-accum|--quarter 1 --scene cornell-pt --frames 45|448x368|nopin
sponza-landscape|--quarter 1 --no-hud --scene sponza --frames 2|448x368|nopin
sponza-portrait|--quarter 0 --no-hud --scene sponza --frames 2|368x448|nopin
sponza-flipped|--quarter 3 --no-hud --scene sponza --frames 2|448x368|nopin
sponza-flat-landscape|--quarter 1 --no-hud --scene sponza-flat --frames 2|448x368|nopin
sponza-flat-portrait|--quarter 0 --no-hud --scene sponza-flat --frames 2|368x448|nopin
sponza-fitted-landscape|--quarter 1 --no-hud --scene sponza-fitted --frames 2|448x368|nopin
sponza-fitted-portrait|--quarter 0 --no-hud --scene sponza-fitted --frames 2|368x448|nopin
sponza-fitted-full-landscape|--quarter 1 --no-hud --scene sponza-fitted-full --frames 2|448x368|nopin
sponza-fitted-full-portrait|--quarter 0 --no-hud --scene sponza-fitted-full --frames 2|368x448|nopin
sponza-depth-landscape|--quarter 1 --no-hud --scene sponza --frames 2 --view depth|448x368|nopin
sponza-depth-portrait|--quarter 0 --no-hud --scene sponza --frames 2 --view depth|368x448|nopin
sponza-depth-flipped|--quarter 3 --no-hud --scene sponza --frames 2 --view depth|448x368|nopin
sponza-tiles-landscape|--quarter 1 --no-hud --scene sponza --frames 2 --view tiles|448x368|nopin
sponza-tiles-portrait|--quarter 0 --no-hud --scene sponza --frames 2 --view tiles|368x448|nopin
sponza-tiles-flipped|--quarter 3 --no-hud --scene sponza --frames 2 --view tiles|448x368|nopin
"

# The scenes read their meshes from asset packs of the baked meshes in the tree.
scene_assets=main

# shellcheck source=../../../../tools/render/render_scene.sh
. "$SCRIPT_DIR/../../../../tools/render/render_scene.sh"
render_scene_run "$@"

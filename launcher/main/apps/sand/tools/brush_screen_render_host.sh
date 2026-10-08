#!/usr/bin/env sh
#
# The sand app's brush screen on a host - see brush_screen_render_host.c and
# docs/tools/Render-Harness.md.
set -eu
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

scene_name=brush_screen
scene_includes="main/apps/sand"
scene_sources="
main/util/runtime/tune.c
main/apps/sand/ui/brush_screen.c
main/apps/sand/material.c
main/apps/sand/material_palette.c
main/apps/sand/sand_ui.c
main/ui/ui_bridge.c
main/ui/ui_build.c
main/ui/ui_canvas_marks.c
main/ui/ui_widgets.c
main/apps/sand/ui/sand_theme.c
main/ui/ui_pointer.c
main/ui/ui_snap.c
components/microui/src/microui.c
main/apps/sand/tools/brush_screen_render_host.c
"
scene_renders="
portrait|--quarter 0|368x448
landscape|--quarter 1|448x368
"

. "$SCRIPT_DIR/../../../../tools/render/render_scene.sh"
render_scene_run "$@"

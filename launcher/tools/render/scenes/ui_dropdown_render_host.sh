#!/bin/sh
#
# ui_dropdown() on a host, opened, scrolled and picked from by a scripted
# finger - see ui_dropdown_render_host.c and docs/tools/Render-Harness.md.
set -eu
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

scene_name=ui_dropdown
scene_sources="
main/gfx/gfx.c
main/util/tune.c
main/ui/ui.c
main/ui/ui_build.c
main/ui/ui_canvas_marks.c
main/ui/ui_pointer.c
main/ui/ui_scroll.c
main/ui/ui_widgets.c
components/microui/src/microui.c
tools/ui_dropdown_render_host.c
"
scene_renders="
scrolled-and-picked|--quarter 0|368x448
"

. "$SCRIPT_DIR/render_scene.sh"
render_scene_run "$@"

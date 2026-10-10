#!/usr/bin/env sh
#
# The sand app's title and options screens on a host - see
# sand_menu_render_host.c and docs/tools/Render-Harness.md.
set -eu
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

scene_name=sand_menu
scene_sources="
main/services/tune.c
main/ui/ui.c
main/ui/ui_bridge.c
main/ui/ui_build.c
main/ui/ui_canvas_marks.c
main/ui/ui_icons.c
main/ui/ui_pointer.c
main/ui/ui_snap.c
main/ui/ui_scroll.c
main/apps/sand/sand_menu.c
main/apps/sand/sand_mode_swatches.c
main/ui/ui_widgets.c
main/apps/sand/ui/sand_icons.c
main/apps/sand/ui/sand_theme.c
main/apps/sand/ui/title_screen.c
main/apps/sand/ui/options_screen.c
components/microui/src/microui.c
main/apps/sand/tools/sand_menu_render_host.c
"
scene_assets="main/engine main/apps/sand"
scene_renders="
title-portrait|--quarter 0|368x448
title-landscape|--quarter 1|448x368
options-portrait|--quarter 0 --options|368x448
options-landscape|--quarter 1 --options|448x368
options-sixteen-portrait|--quarter 0 --options --sixteen|368x448
options-pending-portrait|--quarter 0 --options --pending|368x448
options-sixteen-landscape|--quarter 1 --options --sixteen|448x368
options-dither-open-portrait|--quarter 0 --options --sixteen --open-dither --frames 12|368x448
options-dither-open-landscape|--quarter 1 --options --sixteen --open-dither --frames 12|448x368
title-no-pack-portrait|--quarter 0|368x448|nopacks
options-dither-open-no-pack-portrait|--quarter 0 --options --sixteen --open-dither --frames 12|368x448|nopacks
"

. "$SCRIPT_DIR/../../../../tools/render/render_scene.sh"
render_scene_run "$@"

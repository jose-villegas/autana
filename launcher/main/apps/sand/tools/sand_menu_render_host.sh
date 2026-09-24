#!/usr/bin/env sh
#
# The sand app's title and options screens on a host - see
# sand_menu_render_host.c and docs/tools/Render-Harness.md.
set -eu
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

scene_name=sand_menu
scene_sources="
main/gfx/gfx.c
main/util/tune.c
main/ui/ui.c
main/ui/ui_build.c
main/ui/ui_pointer.c
main/ui/ui_scroll.c
main/apps/sand/sand_menu.c
main/apps/sand/sand_mode_swatches.c
main/ui/ui_widgets.c
main/apps/sand/ui/sand_theme.c
main/apps/sand/ui/title_screen.c
main/apps/sand/ui/options_screen.c
components/microui/src/microui.c
main/apps/sand/tools/sand_menu_render_host.c
"
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
"

. "$SCRIPT_DIR/../../../../tools/render_scene.sh"
render_scene_run "$@"

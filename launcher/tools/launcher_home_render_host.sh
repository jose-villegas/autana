#!/bin/sh
#
# Render the home screen in both orientations, so its layout can be judged
# without a flash cycle.
#
#   ./launcher/tools/launcher_home_render_host.sh [-o <dir>]
#
# This is the scene that proves the general path: the real ui layer, real
# microui, several frames and a declared synthetic touch - see
# launcher_home_render_host.c for what it presses and why two frames are
# the floor. The rows are a fixture registered into the real app_registry.c.
#
# Everything this does beyond the declarations below is
# tools/render_scene.sh.

set -eu
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

scene_name=launcher_home
scene_sources="
main/app_registry.c
main/gfx/gfx.c
main/ui/ui.c
main/ui/ui_build.c
main/ui/ui_launcher.c
main/ui/ui_launcher_draw.c
main/ui/ui_pointer.c
main/ui/ui_ridge.c
main/ui/ui_scroll.c
main/util/tune.c
components/microui/src/microui.c
tools/launcher_home_render_host.c
"
scene_defines="-DCONFIG_LAUNCHER_DEVELOPMENT=0"
scene_renders="
landscape|--quarter 1|448x368
landscape-panel|--quarter 1 --panel|368x448
portrait|--quarter 0|368x448
"

# shellcheck source=./render_scene.sh
. "$SCRIPT_DIR/render_scene.sh"
render_scene_run "$@"

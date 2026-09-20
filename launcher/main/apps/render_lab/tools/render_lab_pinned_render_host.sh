#!/bin/sh
#
# Render this app's INTEGER scenes with the fps/title HUD hidden, and pin
# them - render_lab_render_host.sh's own renders stay unpinned because they
# carry that HUD (a double formatted with "%.1f" defeats a pin), and this
# app also has the HUD to check by eye, which is what that script is for.
#
#   ./launcher/main/apps/render_lab/tools/render_lab_pinned_render_host.sh [-o <dir>]
#
# The Cornell box is float throughout and is left out here: Render-Harness.md
# only pins where the pixels are integer-exact, and this app's float scene
# does not qualify.
#
# Everything beyond the declarations below is
# launcher/tools/render_scene.sh, shared with render_lab_render_host.sh.

set -eu
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

scene_name=render_lab_pinned
scene_sources="
main/gfx/gfx.c
main/ui/ui.c
main/ui/ui_build.c
main/ui/ui_pointer.c
main/ui/ui_scroll.c
main/apps/render_lab/app_render_lab.c
main/apps/render_lab/scene_cube.c
main/apps/render_lab/scene_wire.c
main/apps/render_lab/scene_raytrace.c
main/apps/render_lab/wire_pipeline.c
main/apps/render_lab/rt_cornell.c
main/apps/render_lab/render_lab_mode_switch.c
main/apps/render_lab/ui/render_lab_hud_screen.c
main/apps/render_lab/ui/render_lab_menu_screen.c
components/microui/src/microui.c
main/apps/render_lab/tools/render_lab_render_host.c
main/apps/render_lab/tools/render_lab_render_host_heap.c
"
scene_includes="components/small3dlib/include"
scene_defines="-DCONFIG_LAUNCHER_DEVELOPMENT=0"

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
"

# shellcheck source=../../../../tools/render_scene.sh
. "$SCRIPT_DIR/../../../../tools/render_scene.sh"
render_scene_run "$@"

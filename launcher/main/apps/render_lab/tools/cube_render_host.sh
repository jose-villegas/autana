#!/bin/sh
#
# Render this app on a host, with no board and no flash cycle.
#
#   ./launcher/main/apps/render_lab/tools/cube_render_host.sh [-o <dir>]
#
# The app is entered and stepped for a declared number of 16 ms frames, so
# the rasterizer's own output is what lands in the image. Landscape is the
# shipping orientation, so it leads; the panel-native render is the shape
# tools/render_diff.sh compares a device capture against.
#
# Everything this does beyond the declarations below - finding a compiler,
# building, checking each image against its declared size, converting to
# PNG - is launcher/tools/render_scene.sh.

set -eu
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

scene_name=cube
scene_sources="
main/gfx/gfx.c
main/ui/ui.c
main/ui/ui_build.c
main/ui/ui_pointer.c
main/ui/ui_scroll.c
main/apps/render_lab/app_render_lab.c
main/apps/render_lab/scene_cube.c
main/apps/render_lab/render_lab_mode_switch.c
main/apps/render_lab/ui/render_lab_hud_screen.c
main/apps/render_lab/ui/render_lab_menu_screen.c
components/microui/src/microui.c
main/apps/render_lab/tools/cube_render_host.c
"
scene_includes="components/small3dlib/include"
scene_defines="-DCONFIG_LAUNCHER_DEVELOPMENT=0"

# Size-checked but not hash-pinned. The rasterizer is integer throughout,
# but the frame counter this app draws over it is a double printed with
# "%.1f", and the only reason it reads 0.0 here is that 30 frames of 16 ms
# stop 20 ms short of the window that would compute it. A pin would be
# resting on that margin, and would break on a C library that rounds
# differently the moment anyone changes the frame count.
scene_pin=0
scene_renders="
landscape|--quarter 1|448x368
landscape-panel|--quarter 1 --panel|368x448
portrait|--quarter 0|368x448
"

# shellcheck source=../../../../tools/render_scene.sh
. "$SCRIPT_DIR/../../../../tools/render_scene.sh"
render_scene_run "$@"

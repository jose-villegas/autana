#!/bin/sh
#
# Render this app's scenes on a host, with no board and no flash cycle.
#
#   ./launcher/main/apps/render_lab/tools/render_lab_render_host.sh [-o <dir>]
#
# Each declared render starts on one scene (--scene) via
# render_lab_start_scene_key and steps 16 ms frames, full-framebuffer
# layout. Landscape is the shipping orientation, so it leads; a panel-native
# render is the shape tools/render_diff.sh compares a device capture against.
#
# Everything beyond the declarations below - finding a compiler, building,
# checking each image against its declared size, converting to PNG - is
# launcher/tools/render_scene.sh.

set -eu
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

scene_name=render_lab
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
main/apps/render_lab/scene_pathtrace.c
main/apps/render_lab/wire_pipeline.c
main/apps/render_lab/rt_cornell.c
main/apps/render_lab/rt_cornell_scene.c
main/apps/render_lab/rt_geometry.c
main/apps/render_lab/rt_path.c
main/apps/render_lab/render_lab_mode_switch.c
main/apps/render_lab/ui/render_lab_hud_screen.c
main/apps/render_lab/ui/render_lab_menu_screen.c
components/microui/src/microui.c
main/apps/render_lab/tools/render_lab_render_host.c
main/apps/render_lab/tools/render_lab_render_host_heap.c
"
scene_includes="components/small3dlib/include"

# scene_wire.c's enter()/exit() need a working heap_caps_malloc()/free() on
# the host, matched against test/stubs/esp_heap_caps.h's own declarations
# (render_scene.sh puts that directory on the include path already).
# test/heap_arena.c models the device's real heap caps, but its
# malloc/calloc/realloc/free wrapping reaches gfx_init()'s own host branch
# (gfx.c), which allocates the framebuffer with a plain malloc() it expects
# to always succeed - a rendering preview needs the scratch buffers to
# allocate, nothing about their size or placement, so
# render_lab_render_host_heap.c is a plain pass-through instead.
scene_defines="-DCONFIG_LAUNCHER_DEVELOPMENT=0"

# Size-checked but not hash-pinned. The scenes are integer throughout, but
# the fps readout drawn over them is a double printed with "%.1f", and a pin
# would rest on a C library's rounding.
scene_pin=0
scene_renders="
gouraud-landscape|--quarter 1 --scene gouraud|448x368
gouraud-landscape-panel|--quarter 1 --panel --scene gouraud|368x448
gouraud-portrait|--quarter 0 --scene gouraud|368x448
plane-landscape|--quarter 1 --scene plane|448x368
plane-portrait|--quarter 0 --scene plane|368x448
sphere-landscape|--quarter 1 --scene sphere|448x368
sphere-portrait|--quarter 0 --scene sphere|368x448
wirecube-landscape|--quarter 1 --scene cube|448x368
capsule-landscape|--quarter 1 --scene capsule|448x368
plane-title-fading|--quarter 1 --scene plane --frames 110|448x368
plane-title-gone|--quarter 1 --scene plane --frames 140|448x368
cornell-landscape|--quarter 1 --scene cornell --frames 40|448x368
cornell-portrait|--quarter 0 --scene cornell --frames 40|368x448
cornell-pass1|--quarter 1 --scene cornell --frames 2|448x368
cornell-pass2|--quarter 1 --scene cornell --frames 5|448x368
cornell-pass3|--quarter 1 --scene cornell --frames 9|448x368
cornell-pt-landscape|--quarter 1 --scene cornell-pt --frames 60|448x368
cornell-pt-portrait|--quarter 0 --scene cornell-pt --frames 60|368x448
cornell-pt-seed|--quarter 1 --scene cornell-pt --frames 2|448x368
cornell-pt-accum|--quarter 1 --scene cornell-pt --frames 45|448x368
"

# shellcheck source=../../../../tools/render_scene.sh
. "$SCRIPT_DIR/../../../../tools/render_scene.sh"
render_scene_run "$@"

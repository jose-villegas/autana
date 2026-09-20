#!/bin/sh
#
# Render the wire scenes on a host, with no board and no flash cycle.
#
#   ./launcher/main/apps/render_lab/tools/wire_render_host.sh [-o <dir>]
#
# Each declared render starts on one wire primitive (--scene) via
# render_lab_start_scene_index and steps 16 ms frames, full-framebuffer
# layout, the same shape cube_render_host.sh uses. Landscape leads -
# Testing-Guide.md's "measure landscape first".
#
# Everything beyond the declarations below - finding a compiler, building,
# checking each image against its declared size, converting to PNG - is
# launcher/tools/render_scene.sh.

set -eu
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

scene_name=wire
scene_sources="
main/gfx/gfx.c
main/ui/ui.c
main/ui/ui_build.c
main/ui/ui_pointer.c
main/ui/ui_scroll.c
main/apps/render_lab/app_render_lab.c
main/apps/render_lab/scene_cube.c
main/apps/render_lab/scene_wire.c
main/apps/render_lab/wire_pipeline.c
main/apps/render_lab/render_lab_mode_switch.c
main/apps/render_lab/ui/render_lab_hud_screen.c
main/apps/render_lab/ui/render_lab_menu_screen.c
components/microui/src/microui.c
main/apps/render_lab/tools/wire_render_host.c
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

# Integer end to end, but not hash-pinned for the same reason
# cube_render_host.sh gives: the fps readout is a double printed with
# "%.1f", and 30 frames of 16 ms stop 20 ms short of the window that
# computes it, so it reads 0.0 regardless of anything this scene draws.
scene_pin=0
scene_renders="
plane-landscape|--quarter 1 --scene plane|448x368
plane-portrait|--quarter 0 --scene plane|368x448
sphere-landscape|--quarter 1 --scene sphere|448x368
sphere-portrait|--quarter 0 --scene sphere|368x448
cube-landscape|--quarter 1 --scene cube|448x368
capsule-landscape|--quarter 1 --scene capsule|448x368
plane-title-fading|--quarter 1 --scene plane --frames 110|448x368
plane-title-gone|--quarter 1 --scene plane --frames 140|448x368
"

# shellcheck source=../../../../tools/render_scene.sh
. "$SCRIPT_DIR/../../../../tools/render_scene.sh"
render_scene_run "$@"

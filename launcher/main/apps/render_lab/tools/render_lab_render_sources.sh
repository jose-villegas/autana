#!/bin/sh

# scene_wire.c's enter()/exit() need a working heap_caps_malloc()/free() on
# the host, matched against test/stubs/esp_heap_caps.h's own declarations
# (render_scene.sh puts test/ on the include path).
# test/heap_arena.c models the device's real heap caps, but its
# malloc/calloc/realloc/free wrapping reaches gfx_init()'s own host branch
# (gfx.c), which allocates the framebuffer with a plain malloc() it expects
# to always succeed - a rendering preview needs the scratch buffers to
# allocate, nothing about their size or placement, so
# render_lab_render_host_heap.c is a plain pass-through instead.
scene_sources="
main/app_registry.c
main/gfx/gfx.c
main/util/tune.c
main/util/job.c
main/ui/ui.c
main/ui/ui_build.c
main/ui/ui_pointer.c
main/ui/ui_scroll.c
components/microui/src/microui.c
main/apps/render_lab/tools/render_lab_render_host.c
main/apps/render_lab/tools/render_lab_render_host_heap.c
$(CDPATH= cd -- "$SCRIPT_DIR/../../../../" &&
    find main/apps/render_lab \( -type d \( -name tools -o -name tests \) -prune \) -o \
        \( -type f -name '*.c' ! -name 'suite_*.c' -print \) | LC_ALL=C sort)
"
scene_includes="components/small3dlib/include"
scene_defines="-DCONFIG_LAUNCHER_DEVELOPMENT=0"

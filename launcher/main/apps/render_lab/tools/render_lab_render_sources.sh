#!/bin/sh

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

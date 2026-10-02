#include "scene/scene_shell.h"

#include "display/display.h"
#include "gfx/gfx.h"

void
scene_shell_render(uint32_t dt_ms) {
    scene_render(dt_ms, display_quarter_now(), GFX_WIDTH, GFX_HEIGHT);
}

void
scene_shell_compose(uint32_t dt_ms) {
    if (!scene_has_active_camera()) {
        return;
    }
    /* NULL in band mode, which no scene can be upscaled into. */
    const scene_target_t target = {gfx_framebuffer(), GFX_WIDTH, GFX_HEIGHT};
    scene_compose(dt_ms, display_quarter_now(), &target);
    if (target.pixels != NULL) {
        gfx_mark_dirty(0, 0, GFX_WIDTH, GFX_HEIGHT);
    }
}

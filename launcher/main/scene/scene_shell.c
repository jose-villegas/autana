#include "scene/scene_shell.h"

#include "display/display.h"
#include "gfx/gfx.h"
#include "gfx/present/gfx_mode.h"
#include "gfx/present/gfx_present.h"

void
scene_shell_render(uint32_t dt_ms) {
    scene_render(dt_ms, display_quarter_now(), GFX_WIDTH, GFX_HEIGHT);
}

void
scene_shell_compose(uint32_t dt_ms) {
    if (!scene_has_active_camera()) {
        return;
    }
    gfx_color_t* full = gfx_mode_current()->layout == GFX_LAYOUT_FULL_FB ? gfx_framebuffer() : NULL;
    const scene_target_t target = {full, GFX_WIDTH, GFX_HEIGHT, gfx_half_picture()};
    if (scene_compose(dt_ms, display_quarter_now(), &target)) {
        gfx_expand_frame();
    } else if (target.pixels != NULL) {
        gfx_mark_dirty(0, 0, GFX_WIDTH, GFX_HEIGHT);
    }
}

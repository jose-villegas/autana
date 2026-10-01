#include "scene/scene_shell.h"

#include "display/display.h"
#include "gfx/gfx.h"

/* Whether the last compose found a framebuffer: none in band mode, which no
 * scene can draw into. scene_render() runs while the last frame is still
 * being sent, when no gfx call is allowed, so it asks this instead. */
static bool can_draw;

void
scene_shell_render(uint32_t dt_ms) {
    /* scene_render() only needs a destination to exist; it never writes it. */
    static uint16_t placeholder;
    const scene_target_t target = {can_draw ? &placeholder : NULL, GFX_WIDTH, GFX_HEIGHT};
    scene_render(dt_ms, display_shell_quarter(), &target);
}

void
scene_shell_compose(uint32_t dt_ms) {
    if (!scene_has_active_camera()) {
        return;
    }
    const scene_target_t target = {gfx_framebuffer(), GFX_WIDTH, GFX_HEIGHT};
    can_draw = target.pixels != NULL;
    scene_compose(dt_ms, display_shell_quarter(), &target);
    if (can_draw) {
        gfx_mark_dirty(0, 0, GFX_WIDTH, GFX_HEIGHT);
    }
}

#include "display/display.h"
#include "gfx/gfx.h"
#include "scene/scene.h"

/* scene_render() runs while the last frame is still being sent, when no gfx
 * call is allowed, and only needs to know a framebuffer exists: it never
 * writes it. So it is told of the one the last scene_compose() found. */
static uint16_t* last_pixels;

void
scene_shell_render(uint32_t dt_ms) {
    const scene_target_t target = {last_pixels, GFX_WIDTH, GFX_HEIGHT};
    scene_render(dt_ms, display_shell_quarter(), &target);
}

void
scene_shell_compose(uint32_t dt_ms) {
    last_pixels = gfx_framebuffer(); /* NULL in band mode, which no scene can draw into */
    const scene_target_t target = {last_pixels, GFX_WIDTH, GFX_HEIGHT};
    scene_compose(dt_ms, display_shell_quarter(), &target);
    if (target.pixels != NULL) {
        gfx_mark_dirty(0, 0, GFX_WIDTH, GFX_HEIGHT);
    }
}

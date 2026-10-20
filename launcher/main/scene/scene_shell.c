#include "scene/scene_shell.h"

#include <string.h>

#include "display/display.h"
#include "gfx/gfx.h"
#include "gfx/present/gfx_mode.h"
#include "gfx/present/gfx_present.h"
#include "render/context/render_context.h"

/* Band mode keeps no picture, so the scene is drawn as each band is
 * composed; any other layout's band height is 0, a whole picture. */
static void
follow_layout(void) {
    render_context_set_band_rows(render_context_main(), gfx_mode_current()->band_height);
}

void
scene_shell_render(uint32_t dt_ms) {
    follow_layout();
    scene_render(dt_ms, display_quarter_now(), GFX_WIDTH, GFX_HEIGHT);
}

void
scene_shell_compose(uint32_t dt_ms) {
    if (!scene_has_active_camera()) {
        return;
    }
    follow_layout();
    gfx_color_t* full = gfx_mode_has_framebuffer(gfx_mode_current()) ? gfx_framebuffer() : NULL;
    const scene_target_t target = {full, GFX_WIDTH, GFX_HEIGHT, gfx_half_picture()};
    if (scene_compose(dt_ms, display_quarter_now(), &target)) {
        gfx_expand_frame();
    } else {
        gfx_mark_dirty(0, 0, GFX_WIDTH, GFX_HEIGHT);
    }
}

void
scene_shell_compose_band(int row0, int row1, gfx_color_t* target) {
    const gfx_target_t rows = {target, row0, row1 - row0, GFX_WIDTH};
    if (!scene_compose_rows(rows)) {
        memset(target, 0, sizeof(gfx_color_t) * (size_t)GFX_WIDTH * (size_t)(row1 - row0));
    }
}

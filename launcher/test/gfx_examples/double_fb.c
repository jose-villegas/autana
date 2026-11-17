/* Double framebuffer: request the layout on entry and release it on exit. */
#include "gfx/gfx.h"
#include "image/pixel/rgb565.h"

static bool doubled;

static void
effect_enter(void) {
    const gfx_mode_request_t request = {
        .layout = GFX_LAYOUT_DOUBLE_FB,
        .clear_on_send = true, /* frame() starts with draw_clear() of black */
        .clear_colour = RGB565BE(0x000000),
    };
    doubled = gfx_mode_enter(&request)->layout == GFX_LAYOUT_DOUBLE_FB;
}

static void
effect_exit(void) {
    gfx_mode_exit(); /* also turns clear on send off */
}

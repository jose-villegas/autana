/*
 * ui_control_center - the Control Center's frame, ui_begin()/ui_end()
 * included, and the dimmed backdrop it is painted over. The widgets live in
 * ui_control_center_draw.c, split the way ui_launcher.c splits from
 * ui_launcher_draw.c.
 */

#include "ui/ui_control_center.h"

#include "gfx/gfx.h"
#include "ui/ui.h"

#define BACKDROP_SCRIM_ALPHA 230

void
ui_control_center_dim_backdrop(void) {
    gfx_fill_rect_blend(0, 0, GFX_WIDTH, GFX_HEIGHT, gfx_rgb(0x000000), BACKDROP_SCRIM_ALPHA);
}

void
ui_control_center_frame_layout(const input_t* input, const control_center_layout_t* layout) {
    ui_begin(input);
    ui_control_center_draw(ui_context(), layout);
    ui_end(UI_NO_BACKGROUND);
}

void
ui_control_center_frame(const input_t* input) {
    ui_control_center_frame_layout(input, ui_control_center_baked_layout());
}

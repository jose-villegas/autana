#include "cube_menu_screen.h"

#include <stdio.h>

#include "gfx/gfx.h"
#include "ui/ui.h"
#include "ui/ui_scroll.h"

#define MENU_BTN_W   300
#define MENU_BTN_H   UI_ROW_HEIGHT
#define MENU_BTN_GAP 20

cube_menu_screen_result_t
cube_menu_screen_draw(mu_Context* ctx, const cube_menu_screen_state_t* state, uint32_t dt_ms) {
    cube_menu_screen_result_t result = {0};

    /* ui_set_button_style(UI_BUTTON_BEZEL) is required, not automatic -
     * ui_begin() resets the button style to UI_BUTTON_FLAT every frame
     * (see ui.h), so a bezelled button needs asking for on every frame
     * that draws one, the same as ui_launcher.c's own menu does. */
    ui_set_button_style(UI_BUTTON_BEZEL);

    const int opt = MU_OPT_NOTITLE | MU_OPT_NORESIZE | MU_OPT_NOCLOSE | MU_OPT_NOFRAME;
    if (ui_scroll_view_begin(ctx, "Cube Menu", opt, ui_scroll_view_default(), dt_ms)) {
        const int hint_h = gfx_text_height() + 4;
        const int total_h = 2 * MENU_BTN_H + 2 * MENU_BTN_GAP + hint_h;
        const int top = (ui_height() - total_h) / 2;
        ui_flow_t flow = ui_flow_start(ui_width(), top, MENU_BTN_GAP);

        char label[24];
        snprintf(label, sizeof label, "PARTIAL UPDATES: %s", state->partial_updates_on ? "ON" : "OFF");
        ui_flow_row(ctx, &flow, MENU_BTN_W, MENU_BTN_H);
        if (mu_button(ctx, label)) {
            result.partial_updates_clicked = true;
        }

        snprintf(label, sizeof label, "BAND MODE: %s", state->band_mode_on ? "ON" : "OFF");
        ui_flow_row(ctx, &flow, MENU_BTN_W, MENU_BTN_H);
        if (mu_button(ctx, label)) {
            result.band_mode_clicked = true;
        }

        ui_flow_row(ctx, &flow, MENU_BTN_W, hint_h);
        mu_label(ctx, "BOOT to close");

        ui_scroll_view_end(ctx);
    }

    return result;
}

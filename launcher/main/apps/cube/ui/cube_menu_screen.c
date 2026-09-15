#include "cube_menu_screen.h"

#include <stdio.h>

#include "gfx/gfx.h"
#include "ui/ui.h"

#define MENU_BTN_W   300
#define MENU_BTN_H   UI_ROW_HEIGHT
#define MENU_BTN_GAP 20

cube_menu_screen_result_t
cube_menu_screen_draw(mu_Context* ctx, const cube_menu_screen_state_t* state) {
    cube_menu_screen_result_t result = {0};

    /* ui_set_button_style(UI_BUTTON_BEZEL) is required, not automatic -
     * ui_begin() resets the button style to UI_BUTTON_FLAT every frame
     * (see ui.h), so a bezelled button needs asking for on every frame
     * that draws one, the same as ui_launcher.c's own menu does. */
    ui_set_button_style(UI_BUTTON_BEZEL);

    if (ui_begin_screen(ctx, "Cube Menu", MU_OPT_NOTITLE | MU_OPT_NORESIZE | MU_OPT_NOCLOSE | MU_OPT_NOFRAME)) {
        const int hint_h = gfx_text_height() + 4;
        const int total_h = 2 * MENU_BTN_H + 2 * MENU_BTN_GAP + hint_h;
        const int top = (ui_height() - total_h) / 2;

        char label[24];
        snprintf(label, sizeof label, "PARTIAL UPDATES: %s", state->partial_updates_on ? "ON" : "OFF");

        /* Both the button and the hint below it are placed via
         * mu_layout_set_next() at an absolute rect rather than through
         * mu_layout_row()'s normal top-down flow, the same trick
         * app_sand.c's own two-button boot menu uses: a single small
         * control centered mid-screen has no natural row to sit in. */
        mu_layout_set_next(ctx, ui_centered_rect(ui_width(), MENU_BTN_W, MENU_BTN_H, top), 0);
        if (mu_button(ctx, label)) {
            result.partial_updates_clicked = true;
        }

        snprintf(label, sizeof label, "BAND MODE: %s", state->band_mode_on ? "ON" : "OFF");
        mu_layout_set_next(ctx, ui_centered_rect(ui_width(), MENU_BTN_W, MENU_BTN_H, top + MENU_BTN_H + MENU_BTN_GAP),
                           0);
        if (mu_button(ctx, label)) {
            result.band_mode_clicked = true;
        }

        mu_layout_set_next(ctx, ui_centered_rect(ui_width(), MENU_BTN_W, hint_h, top + 2 * (MENU_BTN_H + MENU_BTN_GAP)),
                           0);
        mu_label(ctx, "BOOT to close");

        mu_end_window(ctx);
    }

    return result;
}

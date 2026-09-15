#include "sand_menu_screen.h"

#include "ui/ui.h"

#define MENU_BTN_W   300
#define MENU_BTN_H   UI_ROW_HEIGHT
#define MENU_BTN_GAP 20

int
sand_menu_screen_row_count(bool show_dither) {
    return show_dither ? 4 : 3;
}

mu_Rect
sand_menu_screen_start_rect(bool show_dither) {
    const int rows = sand_menu_screen_row_count(show_dither);
    const int total_h = rows * MENU_BTN_H + (rows - 1) * MENU_BTN_GAP;
    const int top = (ui_height() - total_h) / 2;
    return ui_centered_rect(ui_width(), MENU_BTN_W, MENU_BTN_H, top);
}

sand_menu_screen_result_t
sand_menu_screen_draw(mu_Context* ctx, const sand_menu_screen_state_t* state) {
    sand_menu_screen_result_t result = {0};

    if (ui_begin_screen(ctx, "Sand Menu", MU_OPT_NOTITLE | MU_OPT_NORESIZE | MU_OPT_NOCLOSE | MU_OPT_NOFRAME)) {
        const int rows = sand_menu_screen_row_count(state->show_dither);
        const int total_h = rows * MENU_BTN_H + (rows - 1) * MENU_BTN_GAP;
        const int top = (ui_height() - total_h) / 2;
        int row = 0;

        mu_layout_set_next(ctx, sand_menu_screen_start_rect(state->show_dither), 0);
        if (mu_button(ctx, "START")) {
            result.start_clicked = true;
        }
        row++;

        mu_layout_set_next(
            ctx, ui_centered_rect(ui_width(), MENU_BTN_W, MENU_BTN_H, top + row * (MENU_BTN_H + MENU_BTN_GAP)), 0);
        if (mu_button(ctx, state->quality)) {
            result.quality_clicked = true;
        }
        row++;

        mu_layout_set_next(
            ctx, ui_centered_rect(ui_width(), MENU_BTN_W, MENU_BTN_H, top + row * (MENU_BTN_H + MENU_BTN_GAP)), 0);
        if (mu_button(ctx, state->color)) {
            result.color_clicked = true;
        }
        row++;

        if (state->show_dither) {
            mu_layout_set_next(
                ctx, ui_centered_rect(ui_width(), MENU_BTN_W, MENU_BTN_H, top + row * (MENU_BTN_H + MENU_BTN_GAP)), 0);
            if (mu_button(ctx, state->dither)) {
                result.dither_clicked = true;
            }
        }

        mu_end_window(ctx);
    }

    return result;
}

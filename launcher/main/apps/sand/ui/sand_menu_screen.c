#include "sand_menu_screen.h"

#include "ui/ui.h"

#define MENU_BTN_W   300
#define MENU_BTN_H   UI_ROW_HEIGHT
#define MENU_BTN_GAP 20

int
sand_menu_screen_row_count(bool show_dither, bool show_two_core) {
    return 3 + (show_dither ? 1 : 0) + (show_two_core ? 1 : 0);
}

mu_Rect
sand_menu_screen_start_rect(bool show_dither, bool show_two_core) {
    const int rows = sand_menu_screen_row_count(show_dither, show_two_core);
    const int total_h = rows * MENU_BTN_H + (rows - 1) * MENU_BTN_GAP;
    const int top = (ui_height() - total_h) / 2;
    return ui_centered_rect(ui_width(), MENU_BTN_W, MENU_BTN_H, top);
}

/* One optional row (DITHER, TWO-CORE) - drawn and reported clicked only
 * when `show` is set, so a caller can lay every row out at a flat
 * sequence of `row` values without branching on each one twice. */
static bool
draw_optional_row(mu_Context* ctx, int top, int row, bool show, const char* label) {
    if (!show) {
        return false;
    }
    mu_layout_set_next(
        ctx, ui_centered_rect(ui_width(), MENU_BTN_W, MENU_BTN_H, top + row * (MENU_BTN_H + MENU_BTN_GAP)), 0);
    return mu_button(ctx, label) != 0;
}

sand_menu_screen_result_t
sand_menu_screen_draw(mu_Context* ctx, const sand_menu_screen_state_t* state) {
    sand_menu_screen_result_t result = {0};

    if (ui_begin_screen(ctx, "Sand Menu", MU_OPT_NOTITLE | MU_OPT_NORESIZE | MU_OPT_NOCLOSE | MU_OPT_NOFRAME)) {
        const int rows = sand_menu_screen_row_count(state->show_dither, state->show_two_core);
        const int total_h = rows * MENU_BTN_H + (rows - 1) * MENU_BTN_GAP;
        const int top = (ui_height() - total_h) / 2;

        mu_layout_set_next(ctx, sand_menu_screen_start_rect(state->show_dither, state->show_two_core), 0);
        result.start_clicked = mu_button(ctx, "START") != 0;

        mu_layout_set_next(ctx, ui_centered_rect(ui_width(), MENU_BTN_W, MENU_BTN_H, top + (MENU_BTN_H + MENU_BTN_GAP)),
                           0);
        result.quality_clicked = mu_button(ctx, state->quality) != 0;

        mu_layout_set_next(
            ctx, ui_centered_rect(ui_width(), MENU_BTN_W, MENU_BTN_H, top + 2 * (MENU_BTN_H + MENU_BTN_GAP)), 0);
        result.color_clicked = mu_button(ctx, state->color) != 0;

        result.dither_clicked = draw_optional_row(ctx, top, 3, state->show_dither, state->dither);
        result.two_core_clicked =
            draw_optional_row(ctx, top, state->show_dither ? 4 : 3, state->show_two_core, state->two_core);

        mu_end_window(ctx);
    }

    return result;
}

#include "sand_menu_screen.h"

#include "ui/ui.h"

#define MENU_BTN_W   300
#define MENU_BTN_H   UI_ROW_HEIGHT
#define MENU_BTN_GAP 20

/* The gap the rows actually get: enough rows at MENU_BTN_GAP overflow the
 * short side, and a row past the edge is a control nobody can reach. */
static int
menu_gap(int rows) {
    const int spare = ui_height() - 2 * UI_MARGIN - rows * MENU_BTN_H;
    const int gaps = (rows > 1) ? rows - 1 : 1;
    const int fits = spare / gaps;
    return (fits < MENU_BTN_GAP) ? ((fits > 0) ? fits : 0) : MENU_BTN_GAP;
}

static int
menu_top(int rows) {
    const int total_h = rows * MENU_BTN_H + (rows - 1) * menu_gap(rows);
    const int top = (ui_height() - total_h) / 2;
    return (top < UI_MARGIN) ? UI_MARGIN : top;
}

int
sand_menu_screen_row_count(bool show_dither) {
    int rows = show_dither ? 4 : 3;
#if CONFIG_LAUNCHER_DEVELOPMENT
    rows++;
#endif
    return rows;
}

mu_Rect
sand_menu_screen_start_rect(bool show_dither) {
    return ui_centered_rect(ui_width(), MENU_BTN_W, MENU_BTN_H, menu_top(sand_menu_screen_row_count(show_dither)));
}

sand_menu_screen_result_t
sand_menu_screen_draw(mu_Context* ctx, const sand_menu_screen_state_t* state) {
    sand_menu_screen_result_t result = {0};
#if CONFIG_LAUNCHER_DEVELOPMENT
    /* Carried forward even if the window below doesn't draw this frame -
     * this is a persistent value, not a one-shot click like the four
     * *_clicked flags {0} above already covers. */
    result.seam_overlay_on = state->seam_overlay_on;
#endif

    if (ui_begin_screen(ctx, "Sand Menu", MU_OPT_NOTITLE | MU_OPT_NORESIZE | MU_OPT_NOCLOSE | MU_OPT_NOFRAME)) {
        const int rows = sand_menu_screen_row_count(state->show_dither);
        const int gap = menu_gap(rows);
        const int top = menu_top(rows);
        int row = 0;

        mu_layout_set_next(ctx, sand_menu_screen_start_rect(state->show_dither), 0);
        if (mu_button(ctx, "START")) {
            result.start_clicked = true;
        }
        row++;

        mu_layout_set_next(ctx, ui_centered_rect(ui_width(), MENU_BTN_W, MENU_BTN_H, top + row * (MENU_BTN_H + gap)),
                           0);
        if (mu_button(ctx, state->quality)) {
            result.quality_clicked = true;
        }
        row++;

        mu_layout_set_next(ctx, ui_centered_rect(ui_width(), MENU_BTN_W, MENU_BTN_H, top + row * (MENU_BTN_H + gap)),
                           0);
        if (mu_button(ctx, state->color)) {
            result.color_clicked = true;
        }
        row++;

        if (state->show_dither) {
            mu_layout_set_next(ctx,
                               ui_centered_rect(ui_width(), MENU_BTN_W, MENU_BTN_H, top + row * (MENU_BTN_H + gap)), 0);
            if (mu_button(ctx, state->dither)) {
                result.dither_clicked = true;
            }
            row++;
        }

#if CONFIG_LAUNCHER_DEVELOPMENT
        mu_layout_set_next(ctx, ui_centered_rect(ui_width(), MENU_BTN_W, MENU_BTN_H, top + row * (MENU_BTN_H + gap)),
                           0);
        int seam_overlay_on = result.seam_overlay_on;
        mu_checkbox(ctx, "show seam stalls (dev)", &seam_overlay_on);
        result.seam_overlay_on = seam_overlay_on;
#endif

        mu_end_window(ctx);
    }

    return result;
}

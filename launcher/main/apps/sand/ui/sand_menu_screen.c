#include "sand_menu_screen.h"

#include "ui/ui.h"

#define MENU_BTN_W   300
#define MENU_BTN_H   UI_ROW_HEIGHT
#define MENU_BTN_GAP 20

/* Where the stack of rows starts: centred when it is shorter than the
 * screen, clamped to UI_MARGIN otherwise - a taller stack now overflows
 * into the window's own scroll instead of needing a shrunk gap to hide it. */
static int
menu_top(int rows) {
    const int total_h = rows * MENU_BTN_H + (rows - 1) * MENU_BTN_GAP;
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
sand_menu_screen_row_rect(int row, int rows) {
    return ui_centered_rect(ui_width(), MENU_BTN_W, MENU_BTN_H, menu_top(rows) + row * (MENU_BTN_H + MENU_BTN_GAP));
}

mu_Rect
sand_menu_screen_start_rect(bool show_dither) {
    return sand_menu_screen_row_rect(0, sand_menu_screen_row_count(show_dither));
}

/* RELATIVE, not ABSOLUTE: still lands at `rest` when scroll is 0 (the
 * container's own body offset is exactly its padding there), but also
 * folds the row into content_size and follows cnt->scroll afterward - see
 * mu_layout_next() in microui.c. That's the whole fix: an ABSOLUTE rect
 * left every row it placed invisible to the container's own scrolling. */
static void
place_row(mu_Context* ctx, mu_Rect rest) {
    const int p = ctx->style->padding;
    mu_layout_set_next(ctx, (mu_Rect){rest.x - p, rest.y - p, rest.w, rest.h}, 1);
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
        int row = 0;

        place_row(ctx, sand_menu_screen_row_rect(row, rows));
        if (mu_button(ctx, "START")) {
            result.start_clicked = true;
        }
        row++;

        place_row(ctx, sand_menu_screen_row_rect(row, rows));
        if (mu_button(ctx, state->quality)) {
            result.quality_clicked = true;
        }
        row++;

        place_row(ctx, sand_menu_screen_row_rect(row, rows));
        if (mu_button(ctx, state->color)) {
            result.color_clicked = true;
        }
        row++;

        if (state->show_dither) {
            place_row(ctx, sand_menu_screen_row_rect(row, rows));
            if (mu_button(ctx, state->dither)) {
                result.dither_clicked = true;
            }
            row++;
        }

#if CONFIG_LAUNCHER_DEVELOPMENT
        place_row(ctx, sand_menu_screen_row_rect(row, rows));
        int seam_overlay_on = result.seam_overlay_on;
        mu_checkbox(ctx, "show seam stalls (dev)", &seam_overlay_on);
        result.seam_overlay_on = seam_overlay_on;
#endif

        mu_end_window(ctx);
    }

    return result;
}

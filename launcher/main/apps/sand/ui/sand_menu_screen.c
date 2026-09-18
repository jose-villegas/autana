#include "sand_menu_screen.h"

#include "build_variant.h"
#include "ui/ui.h"
#include "ui/ui_scroll.h"

#define MENU_BTN_W   300
#define MENU_BTN_H   UI_ROW_HEIGHT
#define MENU_BTN_GAP 20

int
sand_menu_screen_row_count(bool show_dither) {
    int rows = show_dither ? 4 : 3;
#if CONFIG_LAUNCHER_DEVELOPMENT
    rows += 2;
#endif
    return rows;
}

mu_Rect
sand_menu_screen_row_rect(int row, int rows) {
    const int top = ui_flow_top(ui_height(), rows, MENU_BTN_H, MENU_BTN_GAP, UI_MARGIN);
    return ui_centered_rect(ui_width(), MENU_BTN_W, MENU_BTN_H, top + row * (MENU_BTN_H + MENU_BTN_GAP));
}

mu_Rect
sand_menu_screen_start_rect(bool show_dither) {
    return sand_menu_screen_row_rect(0, sand_menu_screen_row_count(show_dither));
}

sand_menu_screen_result_t
sand_menu_screen_draw(mu_Context* ctx, const sand_menu_screen_state_t* state, uint32_t dt_ms) {
    sand_menu_screen_result_t result = {0};
#if CONFIG_LAUNCHER_DEVELOPMENT
    /* Carried forward even if the window below doesn't draw this frame -
     * this is a persistent value, not a one-shot click like the four
     * *_clicked flags {0} above already covers. */
    result.seam_overlay_on = state->seam_overlay_on;
#endif

    const int opt = MU_OPT_NOTITLE | MU_OPT_NORESIZE | MU_OPT_NOCLOSE | MU_OPT_NOFRAME;
    if (ui_scroll_view_begin(ctx, "Sand Menu", opt, ui_scroll_view_default(), dt_ms)) {
        const int rows = sand_menu_screen_row_count(state->show_dither);
        const int top = ui_flow_top(ui_height(), rows, MENU_BTN_H, MENU_BTN_GAP, UI_MARGIN);
        ui_flow_t flow = ui_flow_start(ui_width(), top, MENU_BTN_GAP);

        ui_flow_row(ctx, &flow, MENU_BTN_W, MENU_BTN_H);
        if (mu_button(ctx, "START")) {
            result.start_clicked = true;
        }

        ui_flow_row(ctx, &flow, MENU_BTN_W, MENU_BTN_H);
        if (mu_button(ctx, state->quality)) {
            result.quality_clicked = true;
        }

        ui_flow_row(ctx, &flow, MENU_BTN_W, MENU_BTN_H);
        if (mu_button(ctx, state->color)) {
            result.color_clicked = true;
        }

        if (state->show_dither) {
            ui_flow_row(ctx, &flow, MENU_BTN_W, MENU_BTN_H);
            if (mu_button(ctx, state->dither)) {
                result.dither_clicked = true;
            }
        }

#if CONFIG_LAUNCHER_DEVELOPMENT
        ui_flow_row(ctx, &flow, MENU_BTN_W, MENU_BTN_H);
        if (mu_button(ctx, state->two_core)) {
            result.two_core_clicked = true;
        }

        ui_flow_row(ctx, &flow, MENU_BTN_W, MENU_BTN_H);
        int seam_overlay_on = result.seam_overlay_on;
        mu_checkbox(ctx, "show seam stalls (dev)", &seam_overlay_on);
        result.seam_overlay_on = seam_overlay_on;
#endif

        ui_scroll_view_end(ctx);
    }

    return result;
}

#include "render_lab_menu_screen.h"

#include "apps/render_lab/render_lab_view.h"
#include "services/tune.h"
#include "ui/ui.h"

#define MENU_BTN_W 300

/* The hint is a footer window across the canvas bottom, outside the scrolled
 * rows, so a dropdown list opened over them leaves it showing unless the list
 * needs the whole canvas. */
static int
footer_top(void) {
    return ui_height() - UI_MARGIN - ui_text_height();
}

static void
draw_hint(void) {
    const int top = footer_top();
    ui_window((recti_t){0, top, ui_width(), ui_height() - top});
    ui_text((recti_t){(ui_width() - MENU_BTN_W) / 2, top, MENU_BTN_W, ui_text_height()}, "BOOT to close");
}

void
render_lab_menu_caption(int width, const char* text) {
    ui_row_centre(width, ui_text_height() + UI_TEXT_PAD);
    ui_text(ui_next(), text);
}

render_lab_menu_screen_result_t
render_lab_menu_screen_draw(const render_lab_menu_screen_state_t* state) {
    render_lab_menu_screen_result_t result = {0};

    ui_scroll_window((recti_t){0, 0, ui_width(), footer_top()});

    result.family_picked = -1;
    if (state->family_count > 0) {
        int family = state->family;
        render_lab_menu_caption(MENU_BTN_W, "SCENE");
        ui_row_centre(MENU_BTN_W, UI_ROW_HEIGHT);
        if (ui_dropdown(ui_next(), "SCENE", &family, state->families, state->family_count)) {
            result.family_picked = family;
        }
    }

    if (state->controls != NULL) {
        state->controls(MENU_BTN_W);
    }
#if TUNE_ENABLED
    int count = 0;
    const char* const* names = render_lab_small_fill_names(&count);
    int fill = render_lab_small_fill_selected();
    render_lab_menu_caption(MENU_BTN_W, "SMALL FILL");
    ui_row_centre(MENU_BTN_W, UI_ROW_HEIGHT);
    if (ui_dropdown(ui_next(), "SMALL FILL", &fill, names, count)) {
        (void)render_lab_small_fill_select(fill);
    }
#endif

    draw_hint();

    return result;
}

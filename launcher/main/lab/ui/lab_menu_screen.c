#include "lab_menu_screen.h"

#include "ui/ui.h"

#define MENU_BTN_W    300

/* Each row's caption, and the dropdown identity under it. */
#define SCENE_LABEL   "SCENE:"
#define VARIANT_LABEL "VARIANT:"

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
lab_menu_caption(int width, const char* text) {
    ui_row_centre(width, ui_text_height() + UI_TEXT_PAD);
    ui_text(ui_next(), text);
}

lab_menu_screen_result_t
lab_menu_screen_draw(const lab_menu_screen_state_t* state) {
    lab_menu_screen_result_t result = {0};

    ui_scroll_window((recti_t){0, 0, ui_width(), footer_top()});

    result.family_picked = -1;
    result.variant_picked = -1;
    if (state->family_count > 0) {
        int family = state->family;
        lab_menu_caption(MENU_BTN_W, SCENE_LABEL);
        ui_row_centre(MENU_BTN_W, UI_ROW_HEIGHT);
        if (ui_dropdown(ui_next(), SCENE_LABEL, &family, state->families, state->family_count)) {
            result.family_picked = family;
        }
    }

    if (state->variant_count > 1) {
        int variant = state->variant;
        lab_menu_caption(MENU_BTN_W, VARIANT_LABEL);
        ui_row_centre(MENU_BTN_W, UI_ROW_HEIGHT);
        if (ui_dropdown(ui_next(), VARIANT_LABEL, &variant, state->variants, state->variant_count)) {
            result.variant_picked = variant;
        }
    }

    if (state->controls != NULL) {
        state->controls(MENU_BTN_W);
    }
    if (state->settings != NULL) {
        state->settings(MENU_BTN_W);
    }
    draw_hint();

    return result;
}

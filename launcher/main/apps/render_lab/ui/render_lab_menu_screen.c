#include "render_lab_menu_screen.h"

#include <stdio.h>

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

render_lab_menu_screen_result_t
render_lab_menu_screen_draw(const render_lab_menu_screen_state_t* state) {
    render_lab_menu_screen_result_t result = {0};

    ui_scroll_window((recti_t){0, 0, ui_width(), footer_top()});

    /* The scene name is shown, not part of the identity. */
    char label[48];
    (void)snprintf(label, sizeof label, "NEXT: %s###next scene", state->scene_name);
    ui_row_centre(MENU_BTN_W, UI_ROW_HEIGHT);
    if (ui_button(ui_next(), label)) {
        result.next_scene_clicked = true;
    }

    if (state->controls != NULL) {
        state->controls(MENU_BTN_W);
    }
    draw_hint();

    return result;
}

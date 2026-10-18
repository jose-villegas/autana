#include "render_lab_menu_screen.h"

#include <stdio.h>

#include "ui/ui.h"

#define MENU_BTN_W 300

render_lab_menu_screen_result_t
render_lab_menu_screen_draw(const render_lab_menu_screen_state_t* state) {
    render_lab_menu_screen_result_t result = {0};

    ui_scroll_window((recti_t){0, 0, ui_width(), ui_height()});

    /* The scene name is shown, not part of the identity. */
    char label[48];
    snprintf(label, sizeof label, "NEXT SCENE: %s###next scene", state->scene_name);
    ui_row_centre(MENU_BTN_W, UI_ROW_HEIGHT);
    if (ui_button(ui_next(), label)) {
        result.next_scene_clicked = true;
    }

    if (state->controls != NULL) {
        state->controls(MENU_BTN_W);
    }
    ui_row_centre(MENU_BTN_W, ui_text_height() + UI_TEXT_PAD);
    ui_text(ui_next(), "BOOT to close");

    return result;
}

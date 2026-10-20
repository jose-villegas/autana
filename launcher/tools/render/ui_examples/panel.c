/* A panel grouping a caption and a control, inviting no press itself. */
#include "ui/ui.h"

typedef struct {
    int brightness;
} panel_example_t;

bool
panel_example(panel_example_t* s) {
    ui_row(UI_TITLE_HEIGHT + UI_ROW_HEIGHT);
    const recti_t r = ui_next();
    const int inner_w = r.w - 2 * UI_PADDING;
    ui_panel(r);
    ui_caption((recti_t){r.x + UI_PADDING, r.y, inner_w, UI_TITLE_HEIGHT}, "BRIGHTNESS");
    return ui_slider((recti_t){r.x + UI_PADDING, r.y + UI_TITLE_HEIGHT, inner_w, UI_TAP_MIN}, "brightness",
                     &s->brightness, 0, 4);
}

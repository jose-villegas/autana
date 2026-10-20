/* Icons in the text colour: a pack icon with a short fallback, and the system check. */
#include "ui/controls/ui_icons.h"
#include "ui/ui.h"

typedef struct {
    bool connecting;
} icon_example_t;

bool
icon_example(icon_example_t* s) {
    ui_row(UI_ROW_HEIGHT);
    const recti_t r = ui_next();
    const int side = r.h;
    ui_icon((recti_t){r.x, r.y, side, side}, ui_icon_image(UI_ICON_WIFI), "W");
    ui_text((recti_t){r.x + side + UI_ROW_GAP, r.y, r.w - 2 * (side + UI_ROW_GAP), side}, "NETWORK");
    if (!s->connecting) {
        ui_check((recti_t){r.x + r.w - side, r.y, side, side});
    }
    return false;
}

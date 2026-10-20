/* A tile with an icon above its label, standing for one setting: tapping it
 * flips whether it is selected. */
#include "ui/controls/ui_icons.h"
#include "ui/ui.h"

typedef struct {
    bool wifi;
} tile_icon_example_t;

bool
tile_icon_example(tile_icon_example_t* s) {
    ui_row_centre(2 * UI_ROW_HEIGHT, 2 * UI_ROW_HEIGHT);
    if (ui_tile_icon(ui_next(), "WIFI", ui_icon_image(UI_ICON_WIFI), s->wifi)) {
        s->wifi = !s->wifi;
        return true;
    }
    return false;
}

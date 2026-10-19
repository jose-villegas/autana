/* Tiles with an icon above the label, one choice of three. */
#include "ui/controls/ui_icons.h"
#include "ui/ui.h"

typedef struct {
    int page;
} tile_icon_example_t;

static const char* const PAGES[] = {"HOME", "WIFI", "GAMES"};

bool
tile_icon_example(tile_icon_example_t* s) {
    const draw_image_t* const icons[] = {ui_icon_image(UI_ICON_HOME), ui_icon_image(UI_ICON_WIFI),
                                         ui_icon_image(UI_ICON_GAMEPAD)};
    ui_row(2 * UI_ROW_HEIGHT);
    const recti_t row = ui_next();
    const int w = (row.w - 2 * UI_ROW_GAP) / 3;
    bool picked = false;
    for (int i = 0; i < 3; i++) {
        const recti_t r = {row.x + i * (w + UI_ROW_GAP), row.y, w, row.h};
        if (ui_tile_icon(r, PAGES[i], icons[i], s->page == i)) {
            s->page = i;
            picked = true;
        }
    }
    return picked;
}

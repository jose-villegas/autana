/* A row of tiles, one choice of three, each with the caller's own picture of
 * its choice in the square the tile keeps above its label. */
#include "ui/ui.h"

typedef struct {
    int colour;
} tile_example_t;

static const char* const NAMES[] = {"GOLD", "SEA", "MOSS"};
static const uint32_t COLOURS[] = {0xE0C080, 0x2A6FB0, 0x4F8A3C};

bool
tile_example(tile_example_t* s) {
    ui_row(2 * UI_ROW_HEIGHT);
    const recti_t row = ui_next();
    const int w = (row.w - 2 * UI_ROW_GAP) / 3;
    bool picked = false;
    for (int i = 0; i < 3; i++) {
        const recti_t r = {row.x + i * (w + UI_ROW_GAP), row.y, w, row.h};
        if (ui_tile(r, NAMES[i], s->colour == i)) {
            s->colour = i;
            picked = true;
        }
        ui_fill(ui_tile_icon_rect(r), COLOURS[i]);
    }
    return picked;
}

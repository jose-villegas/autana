/* A row of tiles, one choice of three: the caller says which is selected. */
#include "ui/ui.h"

typedef struct {
    int speed;
} tile_example_t;

static const char* const SPEEDS[] = {"SLOW", "FAST", "WILD"};

bool
tile_example(tile_example_t* s) {
    ui_row(UI_ROW_HEIGHT);
    const recti_t row = ui_next();
    const int w = (row.w - 2 * UI_ROW_GAP) / 3;
    bool picked = false;
    for (int i = 0; i < 3; i++) {
        const recti_t r = {row.x + i * (w + UI_ROW_GAP), row.y, w, row.h};
        if (ui_tile(r, SPEEDS[i], s->speed == i)) {
            s->speed = i;
            picked = true;
        }
    }
    return picked;
}

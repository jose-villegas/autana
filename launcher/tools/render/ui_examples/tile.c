/* Two tiles side by side, one choice of two: the caller says which is selected. */
#include "ui/ui.h"

typedef struct {
    bool hard;
} tile_example_t;

bool
tile_example(tile_example_t* s) {
    ui_row(UI_ROW_HEIGHT);
    const recti_t row = ui_next();
    const int w = (row.w - UI_ROW_GAP) / 2;
    if (ui_tile((recti_t){row.x, row.y, w, row.h}, "EASY", !s->hard)) {
        s->hard = false;
        return true;
    }
    if (ui_tile((recti_t){row.x + w + UI_ROW_GAP, row.y, w, row.h}, "HARD", s->hard)) {
        s->hard = true;
        return true;
    }
    return false;
}

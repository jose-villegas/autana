#include "ui/ui_canvas_marks.h"

#include <string.h>

void
ui_canvas_marks_reset(ui_canvas_marks_t* marks) {
    memset(marks, 0, sizeof *marks);
}

void
ui_canvas_marks_painted(ui_canvas_marks_t* marks, int slot, mu_Rect physical) {
    if (slot < 0 || slot >= MU_CONTAINERPOOL_SIZE) {
        return;
    }
    marks->rect[slot] = physical;
    marks->painted[slot] = true;
}

int
ui_canvas_marks_take_vanished(ui_canvas_marks_t* marks, const bool present[MU_CONTAINERPOOL_SIZE], mu_Rect* out,
                              int max) {
    int n = 0;
    for (int slot = 0; slot < MU_CONTAINERPOOL_SIZE && n < max; slot++) {
        if (marks->painted[slot] && !present[slot]) {
            out[n++] = marks->rect[slot];
            marks->painted[slot] = false;
        }
    }
    return n;
}

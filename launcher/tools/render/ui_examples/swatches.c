/* A grid of colours, with the caller's pick outlined over it. */
#include "ui/ui.h"

typedef struct {
    int picked;
} swatches_example_t;

#define COLS 8
#define ROWS 2

static const uint32_t PALETTE[COLS * ROWS] = {
    0x1D2B53, 0x7E2553, 0x008751, 0xAB5236, 0x5F574F, 0xC2C3C7, 0xFFF1E8, 0xFF004D,
    0xFFA300, 0xFFEC27, 0x00E436, 0x29ADFF, 0x83769C, 0xFF77A8, 0xFFCCAA, 0x000000,
};

bool
swatches_example(swatches_example_t* s) {
    ui_row(UI_ROW_HEIGHT);
    const recti_t r = ui_next();
    const int w = r.w / COLS;
    const int h = r.h / ROWS;
    ui_swatches(r, PALETTE, COLS, ROWS);
    ui_outline((recti_t){r.x + s->picked % COLS * w, r.y + s->picked / COLS * h, w, h}, 0xFFFFFF);
    return false;
}

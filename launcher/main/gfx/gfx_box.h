/*
 * gfx_box: half-open pixel bounds shared by drawing and scene coverage.
 * Inline extension keeps bounds accumulation on the caller's hot path.
 */
#pragma once

#include <stdbool.h>

typedef struct {
    int x0, y0, x1, y1;
} gfx_box_t;

/* An invalid accumulator takes the first box verbatim; its old coordinates
 * need not describe an empty rectangle or be initialized. */
static inline void
gfx_box_extend(gfx_box_t* box, bool valid, int x0, int y0, int x1, int y1) {
    if (!valid) {
        *box = (gfx_box_t){x0, y0, x1, y1};
        return;
    }
    if (x0 < box->x0) {
        box->x0 = x0;
    }
    if (y0 < box->y0) {
        box->y0 = y0;
    }
    if (x1 > box->x1) {
        box->x1 = x1;
    }
    if (y1 > box->y1) {
        box->y1 = y1;
    }
}

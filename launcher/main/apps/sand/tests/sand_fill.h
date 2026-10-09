/* Grid fixture box fills with exclusive upper bounds and row-major writes. */
#pragma once

#include "apps/sand/sand.h"

static inline void
sand_fill_box(sand_t* g, int x0, int y0, int x1, int y1, cell_t c) {
    for (int y = y0; y < y1; y++) {
        for (int x = x0; x < x1; x++) {
            sand_set(g, x, y, c);
        }
    }
}

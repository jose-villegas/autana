/* display_size: the panel's pixels in millimetres, from its diagonal.
 * Header-only and ESP-IDF-free. */
#pragma once

#include <math.h>

#include "board/board_geometry.h"
#include "gfx/gfx.h"

/* Square pixels: the diagonal over the diagonal's pixel count. */
static inline float
display_mm_per_pixel(void) {
    return BOARD_PANEL_DIAGONAL_MM / sqrtf((float)((GFX_WIDTH * GFX_WIDTH) + (GFX_HEIGHT * GFX_HEIGHT)));
}

/* The panel's width and height, upright, in millimetres. */
static inline float
display_width_mm(void) {
    return (float)GFX_WIDTH * display_mm_per_pixel();
}

static inline float
display_height_mm(void) {
    return (float)GFX_HEIGHT * display_mm_per_pixel();
}

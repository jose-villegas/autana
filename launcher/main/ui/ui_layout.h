/* Rectangle storage shared by authored screen tables. */
#pragma once

#include <stdint.h>

typedef struct {
    int16_t x, y, width, height;
} ui_layout_rect_t;

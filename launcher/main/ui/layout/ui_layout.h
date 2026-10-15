/* Rectangle storage shared by authored screen tables. */
#pragma once

#include <stdint.h>

#include "math/geometry/recti.h"

typedef struct {
    int16_t x, y, width, height;
} ui_layout_rect_t;

static inline recti_t
ui_layout_rect_to_recti(ui_layout_rect_t r) {
    return (recti_t){r.x, r.y, r.width, r.height};
}

/* Snap a touch outside controls to the closest reachable control edge. */
#pragma once

#include <stdbool.h>

#include "microui.h"

/* Most controls one frame may draw, with room for screen growth. */
#define UI_SNAP_RECTS_MAX 20

typedef struct {
    mu_Rect r;
    bool live;
} ui_snap_rect_t;

/* Returns the unchanged point when it is inside a rect, no rect is reachable,
 * or reach is zero. Equal edge distances retain rect order. */
mu_Vec2 ui_snap_point(const ui_snap_rect_t* rects, int count, mu_Vec2 point, int reach);

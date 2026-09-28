/* Snap a touch outside controls to the closest reachable control edge. */
#pragma once

#include "microui.h"

/* The largest screen is the material palette. */
#define UI_SNAP_RECTS_MAX 15

/* Returns the unchanged point when it is inside a rect, no rect is reachable,
 * or reach is zero. Equal edge distances retain rect order. */
mu_Vec2 ui_snap_point(const mu_Rect* rects, int count, mu_Vec2 point, int reach);

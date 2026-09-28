/* Snap a touch outside controls to the closest reachable control edge. */
#pragma once

#include "microui.h"

/* The UI keeps this many controls from its completed frame. Extra controls
 * are drawn normally but cannot be snap candidates until capacity is raised. */
#define UI_SNAP_RECTS_MAX 32

/* Returns the unchanged point when it is inside a rect, no rect is reachable,
 * or reach is zero. Equal edge distances retain rect order. */
mu_Vec2 ui_snap_point(const mu_Rect* rects, int count, mu_Vec2 point, int reach);

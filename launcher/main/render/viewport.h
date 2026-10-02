/*
 * viewport: the picture a camera is drawn into, and how the panel's own
 * axes lie in it once the shell reads the panel at a quarter turn.
 * Header-only and ESP-IDF-free.
 */
#pragma once

#include <stdbool.h>

typedef struct {
    int width;
    int height;
    int quarter; /* 0..3, as display_quarter_now() numbers a turn */
} viewport_t;

/* How the panel's own axes lie in the upright picture once it is read at
 * `quarter`: a step along physical x moves the upright pixel by (x_right,
 * x_down), a step along physical y by (y_right, y_down). */
typedef struct {
    int x_right, x_down, y_right, y_down;
} viewport_quarter_axes_t;

static inline viewport_quarter_axes_t
viewport_quarter_axes(int quarter) {
    switch (quarter & 3) {
        case 1: return (viewport_quarter_axes_t){0, -1, 1, 0};
        case 2: return (viewport_quarter_axes_t){-1, 0, 0, -1};
        case 3: return (viewport_quarter_axes_t){0, 1, -1, 0};
        default: return (viewport_quarter_axes_t){1, 0, 0, 1};
    }
}

/* The inverse of ui_transform_quarter_turn(): where in the upright picture
 * a physical pixel lands once the panel is read at `viewport.quarter`.
 * Spelled out here rather than included, since render/ sits below ui/. */
static inline void
viewport_physical_to_upright(viewport_t viewport, int px, int py, int* ux, int* uy) {
    const viewport_quarter_axes_t a = viewport_quarter_axes(viewport.quarter);
    const bool swapped = a.x_right == 0;
    const int upright_width = swapped ? viewport.height : viewport.width;
    const int upright_height = swapped ? viewport.width : viewport.height;
    *ux = (a.x_right + a.y_right < 0 ? upright_width - 1 : 0) + a.x_right * px + a.y_right * py;
    *uy = (a.x_down + a.y_down < 0 ? upright_height - 1 : 0) + a.x_down * px + a.y_down * py;
}

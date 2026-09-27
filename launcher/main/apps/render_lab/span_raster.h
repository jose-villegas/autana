/*
 * span_raster - a depth-tested, Gouraud-shaded triangle filled one scanline
 * span at a time into a caller's window of rows.
 *
 * Setup is float, once per triangle and once per row; the per-pixel loop is
 * integer adds only. Every attribute is sampled from its plane equation at
 * the pixel centre rather than stepped down the edges, so a triangle clipped
 * to a window of rows draws the same pixels as the whole one. Depth is the
 * caller's inverse depth in (0, 1], larger nearer, kept as 16 bits.
 */
#pragma once

#include <stdint.h>

#include "gfx/gfx_color.h"

typedef struct {
    gfx_color_t* color; /* the first pixel of screen row `row0` */
    uint16_t* depth;    /* the same shape as `color`; 0 is infinitely far */
    int width;          /* pixels per row, and the stride of both buffers */
    int row0, row1;     /* the half-open screen rows this window holds */
} span_target_t;

typedef struct {
    float x, y;    /* screen position, pixel centres at +0.5 */
    float z;       /* inverse depth, (0, 1] */
    float r, g, b; /* 0..255 */
} span_vertex_t;

void span_raster_triangle(const span_target_t* target, const span_vertex_t* a, const span_vertex_t* b,
                          const span_vertex_t* c);

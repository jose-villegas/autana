/*
 * span_raster - a depth-tested, Gouraud-shaded triangle filled one scanline
 * span at a time into a caller's window of rows.
 *
 * Setup is float, once per triangle; rows and spans step in fixed point with
 * integer adds. Edges and attributes are anchored at the triangle's own
 * first row, so a triangle clipped to a window of rows draws exactly the
 * pixels of the whole one. Depth is the caller's inverse depth in (0, 1],
 * larger nearer, kept as 16 bits.
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

/* Temporary measurement switch: 0 draws normally; 1 stops after triangle
 * setup, 2 after walking the rows, 3 after each span's setup. */
extern int span_raster_stop_after;

void span_raster_triangle(const span_target_t* target, const span_vertex_t* a, const span_vertex_t* b,
                          const span_vertex_t* c);

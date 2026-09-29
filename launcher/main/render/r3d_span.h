/*
 * r3d_span - a depth-tested, Gouraud-shaded triangle filled into a
 * caller's window of rows. Coverage is the top-left rule on 1/16-pixel
 * positions, decided in integers: triangles sharing an edge never both fill
 * or both miss a pixel. Depth is inverse depth in (0, 1], larger nearer.
 */
#pragma once

#include <stdint.h>

typedef struct {
    /* The first pixel of screen row `row0`. Pixels are the panel's own
     * format, RGB565 with its two bytes swapped, so a framebuffer can be a
     * target as it is. */
    uint16_t* color;
    uint16_t* depth; /* the same shape as `color`; 0 is infinitely far */
    int width;       /* pixels per row, and the stride of both buffers */
    int row0, row1;  /* the half-open screen rows this window holds */
} r3d_span_target_t;

#define R3D_SUBPIXEL       16
/* Positions past this many pixels from the origin are held at it. */
#define R3D_SUBPIXEL_LIMIT 67108864.0f

typedef struct {
    int32_t x, y;  /* screen position in 1/R3D_SUBPIXEL pixels; pixel i's centre is at 16 i + 8 */
    float z;       /* inverse depth, (0, 1] */
    float r, g, b; /* 0..255 */
} r3d_span_vertex_t;

static inline int32_t
r3d_span_snap(float pixels) {
    const float v = pixels < -R3D_SUBPIXEL_LIMIT ? -R3D_SUBPIXEL_LIMIT
                                                 : (pixels > R3D_SUBPIXEL_LIMIT ? R3D_SUBPIXEL_LIMIT : pixels);
    const float scaled = v * (float)R3D_SUBPIXEL;
    return (int32_t)(scaled + (scaled < 0.0F ? -0.5F : 0.5F));
}

/* The first pixel whose centre is at or past subpixel position v. */
static inline int
r3d_span_first_centre(int32_t v) {
    return (v + (R3D_SUBPIXEL / 2) - 1) >> 4;
}

/* Temporary measurement switch: 0 draws normally; 1 stops after triangle
 * setup, 2 after walking the rows, 3 after each span's setup. */
extern int r3d_span_stop_after;

void r3d_span_triangle(const r3d_span_target_t* target, const r3d_span_vertex_t* a, const r3d_span_vertex_t* b,
                       const r3d_span_vertex_t* c);

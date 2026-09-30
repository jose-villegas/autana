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

#define R3D_SUBPIXEL_SHIFT 4
#define R3D_SUBPIXEL       (1 << R3D_SUBPIXEL_SHIFT)
/* Every coordinate r3d_span_triangle() takes is inside +-R3D_SPAN_RANGE
 * subpixels (1024 pixels), which keeps each edge's arithmetic in 32 bits. */
#define R3D_SPAN_RANGE     (1 << 14)
/* Added before truncating, so the sum is positive and truncating floors. */
#define R3D_SNAP_BIAS      ((float)R3D_SPAN_RANGE + 0.5F)

typedef struct {
    int32_t x, y;  /* screen position in subpixels; pixel i's centre is at R3D_SUBPIXEL i + R3D_SUBPIXEL / 2 */
    float z;       /* inverse depth, (0, 1] */
    float r, g, b; /* 0..255 */
} r3d_span_vertex_t;

/* A subpixel position plus R3D_SNAP_BIAS, as a float, to the nearest
 * subpixel. `biased` must lie in (0, 2 R3D_SPAN_RANGE). */
static inline int32_t
r3d_span_unbias(float biased) {
    return (int32_t)biased - R3D_SPAN_RANGE;
}

/* A position in pixels, under 1024 from the origin, to the nearest subpixel. */
static inline int32_t
r3d_span_snap(float pixels) {
    return r3d_span_unbias((pixels * (float)R3D_SUBPIXEL) + R3D_SNAP_BIAS);
}

/* The first pixel whose centre is at or past subpixel position v. */
static inline int
r3d_span_first_centre(int32_t v) {
    return (v + (R3D_SUBPIXEL / 2) - 1) >> R3D_SUBPIXEL_SHIFT;
}

static inline int32_t
r3d_span_min3(int32_t a, int32_t b, int32_t c) {
    return a < b ? (a < c ? a : c) : (b < c ? b : c);
}

static inline int32_t
r3d_span_max3(int32_t a, int32_t b, int32_t c) {
    return a > b ? (a > c ? a : c) : (b > c ? b : c);
}

/* Temporary measurement switch: 0 draws normally; 1 stops after triangle
 * setup, 2 after walking the rows, 3 after each span's setup. A triangle
 * tested centre by centre stops after its setup under any of them. */
extern int r3d_span_stop_after;

void r3d_span_triangle(const r3d_span_target_t* target, const r3d_span_vertex_t* a, const r3d_span_vertex_t* b,
                       const r3d_span_vertex_t* c);

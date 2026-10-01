/*
 * r3d_span: a depth-tested, Gouraud-shaded triangle filled into a
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
    uint16_t* depth; /* the same shape as `color`; R3D_DEPTH_EMPTY where nothing was drawn */
    int width;       /* pixels per row, and the stride of both buffers */
    int row0, row1;  /* the half-open screen rows this window holds */
} r3d_span_target_t;

/* The depth encoding: inverse depth scaled to 16 bits, so a larger value is
 * nearer. A cleared buffer holds R3D_DEPTH_EMPTY, infinitely far, which no
 * drawn pixel can equal. */
#define R3D_DEPTH_EMPTY    0
#define R3D_DEPTH_NEAREST  UINT16_MAX

#define R3D_SUBPIXEL_SHIFT 4
#define R3D_SUBPIXEL       (1 << R3D_SUBPIXEL_SHIFT)
/* Every coordinate r3d_span_triangle() takes is inside +-R3D_SPAN_RANGE
 * subpixels (1024 pixels), which keeps each edge's arithmetic in 32 bits. */
#define R3D_SPAN_RANGE     (1 << 14)

typedef struct {
    int32_t x, y;  /* screen position in subpixels; pixel i's centre is at R3D_SUBPIXEL i + R3D_SUBPIXEL / 2 */
    float z;       /* inverse depth, (0, 1] */
    float r, g, b; /* 0..255 */
} r3d_span_vertex_t;

void r3d_span_triangle(const r3d_span_target_t* target, const r3d_span_vertex_t* a, const r3d_span_vertex_t* b,
                       const r3d_span_vertex_t* c);
void r3d_span_triangle_solid(const r3d_span_target_t* target, const r3d_span_vertex_t* a, const r3d_span_vertex_t* b,
                             const r3d_span_vertex_t* c, uint16_t color);

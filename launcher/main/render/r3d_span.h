/*
 * r3d_span: a depth-tested, Gouraud-shaded triangle filled into a
 * caller's window of rows. Coverage is the top-left rule on 1/16-pixel
 * positions, decided in integers: triangles sharing an edge never both fill
 * or both miss a pixel. Depth is inverse depth in (0, 1], larger nearer.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "gfx/gfx_render_target.h"

typedef struct r3d_span_writer r3d_span_writer_t;

/* Where a triangle is filled: a render target whose colour is the panel's
 * own format, RGB565 with its two bytes swapped, so a framebuffer can be its
 * colour as it is, and whose depth is R3D_DEPTH_EMPTY where nothing was
 * drawn; and what a further attachment writes beside them. */
typedef struct {
    gfx_render_target_t rows;
    const r3d_span_writer_t* writers; /* writer_count of them; none: colour and depth only */
    int writer_count;
} r3d_span_target_t;

/* Colour and depth alone, rows [row0, row1) `width` wide, each buffer
 * starting at row0. */
static inline r3d_span_target_t
r3d_span_target(gfx_color_t* color, uint16_t* depth, int width, int row0, int row1) {
    return (r3d_span_target_t){
        {width, row0, row1, 2, {{color, sizeof(gfx_color_t)}, {depth, sizeof(uint16_t)}}}, NULL, 0};
}

/* A further attachment's part in a fill, chosen once per draw: after each
 * span's colour and depth, `span` is called with the span's depth, 16.8 at
 * x_first and stepping dz a pixel, so it can find the pixels this triangle
 * won (depth == z >> R3D_DEPTH_SHIFT) and write its own there. A map that
 * needs more than depth and a value per draw, such as normals, rebuilds it in
 * its resolve. */
struct r3d_span_writer {
    void (*span)(const r3d_span_writer_t* writer, const gfx_render_target_t* rows, int y, int x_first, int x_last,
                 int32_t z, int32_t dz);
    int attachment;   /* the index it writes, in `rows` */
    bool per_cluster; /* adds the drawn cluster's index to `value` */
    uint32_t value;   /* what this draw writes, in the attachment's own meaning */
};

/* The depth encoding: inverse depth scaled to 16 bits, so a larger value is
 * nearer. A cleared buffer holds R3D_DEPTH_EMPTY, infinitely far, which no
 * drawn pixel can equal. */
#define R3D_DEPTH_EMPTY    0
#define R3D_DEPTH_NEAREST  UINT16_MAX
/* A span walks depth as 16.8: the 16-bit depth is z >> R3D_DEPTH_SHIFT. */
#define R3D_DEPTH_SHIFT    8

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

/* Right after either fill of the same triangle into the same target: its
 * writers over the pixels it covers. A caller with no writers never calls
 * it, so the fill itself carries none of their cost. */
void r3d_span_triangle_write(const r3d_span_target_t* target, const r3d_span_vertex_t* a, const r3d_span_vertex_t* b,
                             const r3d_span_vertex_t* c);

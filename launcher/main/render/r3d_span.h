/*
 * r3d_span: a depth-tested, Gouraud-shaded (Gouraud 1971) triangle
 * filled into a caller's window of rows. Coverage is the top-left rule on
 * 1/16-pixel positions, decided in integers: triangles sharing an edge never
 * both fill or both miss a pixel. Depth is inverse depth in (0, 1], larger
 * nearer.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "gfx/target/gfx_target.h"
#include "render/lmsk_format.h"
#include "render/render_view.h"

/* Largest bounding-box side, in pixel centres, always filled flat; the camera gate and style apply only above it. */
#define R3D_SPAN_SMALL_MAX_SIDE    4

/* Corners of the triangle every fill draws. */
#define R3D_SPAN_TRIANGLE_VERTICES 3

/* Resolved once per frame and shared read-only by row workers. */
typedef struct {
    uint8_t ceiling;
    small_fill_style_t style;
    uint8_t spread[SMALL_FILL_CEILING_MAX + 1];
} r3d_span_small_fill_t;

extern const r3d_span_small_fill_t r3d_span_small_fill_default;
void r3d_span_small_fill_resolve(r3d_span_small_fill_t* resolved, small_fill_t fill);

typedef struct r3d_span_writer r3d_span_writer_t;

/* Where a triangle is filled: a render target whose colour is the panel's
 * own format, RGB565 with its two bytes swapped, so a framebuffer can be its
 * colour as it is, and whose depth is R3D_DEPTH_EMPTY where nothing was
 * drawn; and what a further attachment writes beside them. */
typedef struct {
    const r3d_span_writer_t* writers; /* writer_count of them; none: colour and depth only */
    int writer_count;
    bool skip_hidden; /* render_view_t.skip_hidden: fills cap their depth, and writers follow */
    /* A screen tile R3D_SPAN_SCREEN_TILE pixels square, bit (y % tile) * tile + (x % tile)
     * set where pixels are drawn. 0 draws every pixel; any other value sends every
     * triangle of r3d_pipeline_draw() through the masked fill. */
    uint16_t screen_mask;
    const r3d_span_small_fill_t* small_fill;

    union {
        gfx_target_t rows;
        GFX_TARGET_STORAGE(GFX_ATTACHMENTS_MAX) rows_storage;
    };
} r3d_span_target_t;

/* Colour and depth alone, rows [row0, row1) `width` wide, each buffer
 * starting at row0. */
static inline r3d_span_target_t
r3d_span_target(gfx_color_t* color, uint16_t* depth, int width, int row0, int row1) {
    r3d_span_target_t target = {.small_fill = &r3d_span_small_fill_default};
    gfx_target_of_color_depth(&target.rows, color, depth, width, row1 - row0, row0);
    assert(target.rows.attachment[GFX_ATTACHMENT_DEPTH].pitch == target.rows.attachment[GFX_ATTACHMENT_COLOR].pitch);
    return target;
}

/* A further attachment's part in a fill, chosen once per draw: after each
 * span's colour and depth, `span` is called with the span's depth, 16.8 at
 * x_first and stepping dz a pixel, so it can find the pixels this triangle
 * won (depth == z >> R3D_DEPTH_SHIFT) and write its own there. A map that
 * needs more than depth and a value per draw, such as normals, rebuilds it in
 * its resolve. */
/* What a writer adds to its value for each part it draws. */
typedef enum {
    R3D_SPAN_STEP_NONE,     /* one value for the whole draw */
    R3D_SPAN_STEP_CLUSTER,  /* the drawn cluster's index */
    R3D_SPAN_STEP_TRIANGLE, /* the drawn triangle's index */
} r3d_span_step_t;

struct r3d_span_writer {
    void (*span)(const r3d_span_writer_t* writer, const gfx_target_t* rows, int y, int x_first, int x_last, int32_t z,
                 int32_t dz);
    int attachment;       /* the index it writes, in `rows` */
    r3d_span_step_t step; /* what is added to `value` per draw */
    uint32_t value;       /* what this draw writes, in the attachment's own meaning */
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

/* The side of r3d_span_target_t.screen_mask's tile, in pixels. */
#define R3D_SPAN_SCREEN_TILE 4

/* A masked triangle's mask: its atlas, and its vertices' LMSK 12.4 texel
 * coordinates in the order the fill takes them. Apart from the vertex, so an
 * opaque triangle's vertices carry none. No atlas: the target's screen_mask
 * instead, and no u, v. */
typedef struct {
    const lmsk_atlas_t* atlas;
    float u[R3D_SPAN_TRIANGLE_VERTICES], v[R3D_SPAN_TRIANGLE_VERTICES];
} r3d_span_mask_t;

void r3d_span_triangle(const r3d_span_target_t* target, const r3d_span_vertex_t* a, const r3d_span_vertex_t* b,
                       const r3d_span_vertex_t* c);
void r3d_span_triangle_solid(const r3d_span_target_t* target, const r3d_span_vertex_t* a, const r3d_span_vertex_t* b,
                             const r3d_span_vertex_t* c, uint16_t color);

/* Either fill, `face` NULL or its colour, writing no depth nearer than
 * r3d_span_depth_limit() of the nearest vertex: what a skip of whole clusters
 * behind drawn depth relies on. */
void r3d_span_capped_triangle(const r3d_span_target_t* target, const r3d_span_vertex_t* a, const r3d_span_vertex_t* b,
                              const r3d_span_vertex_t* c, const uint16_t* face);

/* Either fill, `face` NULL or its colour, keeping only the pixels `mask`'s
 * atlas keeps at its u, v, or without an atlas the target's screen_mask
 * keeps; capped as r3d_span_capped_triangle() when
 * target->skip_hidden. The target's writers follow each kept run inside it:
 * no r3d_span_triangle_write() after it. */
void r3d_span_masked_triangle(const r3d_span_target_t* target, const r3d_span_mask_t* mask, const r3d_span_vertex_t* a,
                              const r3d_span_vertex_t* b, const r3d_span_vertex_t* c, const uint16_t* face);

/* Right after either fill of the same triangle into the same target: its
 * writers over the pixels it covers. `solid` must say whether that fill used a
 * face colour: the small-triangle choice depends on it, and the writers must
 * take the same path as the fill, capped when target->skip_hidden.
 * A caller with no writers never calls it, so the fill itself carries none
 * of their cost. */
void r3d_span_triangle_write(const r3d_span_target_t* target, const r3d_span_vertex_t* a, const r3d_span_vertex_t* b,
                             const r3d_span_vertex_t* c, bool solid);

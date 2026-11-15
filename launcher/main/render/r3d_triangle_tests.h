/*
 * r3d_triangle_tests: what the draw asks of a transformed triangle before it
 * fills it, shared by the draw and by the counts beside it
 * (render/view/r3d_pipeline_count.h). Internal to render/.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "render/r3d_pipeline.h"
#include "render/r3d_span_internal.h"

/* Front faces wind negative on screen: counter-clockwise in a y-up world
 * turns clockwise once screen y points down. */
static inline bool
facing_away(int32_t area2, bool double_sided) {
    return !double_sided && area2 >= 0;
}

/* A transformed corner `v` as a span vertex lit `rgb`. */
static inline r3d_span_vertex_t
span_corner(const r3d_pipeline_vertex_t* v, const uint8_t rgb[3]) {
    return (r3d_span_vertex_t){v->sx, v->sy, v->iz, rgb[0], rgb[1], rgb[2]};
}

/* Twice the signed area; with coordinates inside R3D_SPAN_RANGE each
 * product is under 2^30. */
static inline int32_t
signed_area2(int32_t ax, int32_t ay, int32_t bx, int32_t by, int32_t cx, int32_t cy) {
    return ((bx - ax) * (cy - ay)) - ((cx - ax) * (by - ay));
}

/* True when the bounding box holds no pixel centre inside the target, so
 * the rasterizer would fill nothing: most of these are triangles smaller
 * than a pixel falling between centres. Forced inline, for a caller whose
 * frame a call would grow; misses_every_centre() leaves it to the compiler. */
static inline __attribute__((always_inline)) bool
misses_every_centre_inlined(const r3d_pipeline_vertex_t* a, const r3d_pipeline_vertex_t* b,
                            const r3d_pipeline_vertex_t* c, const r3d_span_target_t* target) {
    const recti_t box = r3d_span_centres(r3d_span_min3(a->sx, b->sx, c->sx), r3d_span_min3(a->sy, b->sy, c->sy),
                                         r3d_span_max3(a->sx, b->sx, c->sx), r3d_span_max3(a->sy, b->sy, c->sy));
    return box.w <= 0 || box.h <= 0 || recti_right(box) <= 0 || box.x >= target->rows.attachment[0].width
           || recti_bottom(box) <= target->rows.first_row || box.y >= gfx_target_end_row(&target->rows);
}

/* misses_every_centre_inlined(), inlined where the compiler finds it pays:
 * forcing it everywhere changes the register use of the loops it lands in. */
static inline bool
misses_every_centre(const r3d_pipeline_vertex_t* a, const r3d_pipeline_vertex_t* b, const r3d_pipeline_vertex_t* c,
                    const r3d_span_target_t* target) {
    return misses_every_centre_inlined(a, b, c, target);
}

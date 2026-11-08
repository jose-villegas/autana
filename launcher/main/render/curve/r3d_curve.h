/* r3d_curve: thin tubes drawn as camera-facing ribbons with coverage fading. */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "asset/asset_pack.h"
#include "math/geometry/box3s.h"
#include "math/linear/vec_convert.h"
typedef struct r3d_lens r3d_lens_t;
typedef struct r3d_span_target r3d_span_target_t;

typedef struct {
    int16_t x, y, z;
    uint16_t radius, color;
} r3d_curve_knot_t;

typedef struct {
    uint16_t first, count;
    int16_t lo[3], hi[3];
} r3d_curve_t;

typedef struct {
    const r3d_curve_t* curves;
    const r3d_curve_knot_t* knots;
    int curve_count;
} r3d_curve_set_t;

/* Narrower ribbons fade by their true width divided by this drawn width. */
#define R3D_CURVE_MIN_WIDTH_PX 1.0F
#define R3D_CURVE_ASSET        ASSET_TYPE('L', 'C', 'R', 'V')
#define R3D_CURVE_NAME_SUFFIX  ".crv"
#define R3D_CURVE_HEADER_WORDS 4
#define R3D_CURVE_VERSION      1

/* A row's box, in the mesh's position ticks. */
static inline box3s_t
r3d_curve_box(const r3d_curve_t* curve) {
    return (box3s_t){vec3s_from_array(curve->lo), vec3s_from_array(curve->hi)};
}

/* An LCRV entry's rows, their knots right after them. */
static inline r3d_curve_set_t
r3d_curve_set_at(const r3d_curve_t* curves, int count) {
    if (count == 0) {
        return (r3d_curve_set_t){0};
    }
    return (r3d_curve_set_t){curves, (const r3d_curve_knot_t*)(const void*)(curves + count), count};
}

/* Checks an LCRV entry, header to last knot, and points `out` into it; `out` is empty on failure. */
asset_status_t r3d_curve_set_from_asset(asset_view_t asset, r3d_curve_set_t* out);

/* The opaque segments of `set`, or with `covered` the faded ones. Draw every set's opaque segments first: a faded
 * segment mixes over what is behind it without hiding it. `set` is passed by value so the draw path's stack holds
 * no copy of it. */
void r3d_curve_draw(r3d_curve_set_t set, const r3d_lens_t* lens, const r3d_span_target_t* target, bool covered);

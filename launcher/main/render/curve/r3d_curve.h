/* r3d_curve: thin tubes drawn as camera-facing ribbons with coverage fading. */
#pragma once

#include <stdint.h>
#include "asset/asset_pack.h"
#include "render/r3d_span.h"

typedef struct r3d_lens r3d_lens_t;

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
#define R3D_CURVE_NAME_SUFFIX  ".curves"
#define R3D_CURVE_HEADER_WORDS 4

void r3d_curve_draw(const r3d_curve_set_t* set, const r3d_lens_t* lens, const r3d_span_target_t* target);

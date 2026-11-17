/*
 * affine2x: a 2D affine map of a Q16.16, with integer-pixel maps.
 * The operations are affine2_template.h's; docs/math/Linear.md says how they are made.
 */
#pragma once

#include "math/geometry/recti.h"
#include "math/geometry/rectx.h"
#include "math/linear/affine2_template.h"
#include "math/linear/vec2i.h"
#include "math/linear/vec2x.h"

MATH_DEFINE_AFFINE2(affine2x, vec2x, rectx, int32_t, mathx)

/* pixel = round(L * p + origin), nearest with ties away from zero. */
static inline vec2i_t
affine2x_map_pixel(const affine2x_t* t, vec2i_t p) {
    return (vec2i_t){
        (int32_t)fx_round_shift((int64_t)t->x_axis.x * p.x + (int64_t)t->y_axis.x * p.y + t->origin.x, MATHX_SHIFT),
        (int32_t)fx_round_shift((int64_t)t->x_axis.y * p.x + (int64_t)t->y_axis.y * p.y + t->origin.y, MATHX_SHIFT),
    };
}

MATH_DEFINE_AFFINE2_RECT(affine2x, vec2i, recti, mathi, map_recti, map_pixel)

/* Rotation by `turn` quarter turns clockwise (mod 4, negative allowed) of a
 * frame onto a `w` x `h` one: the domain is h x w for an odd turn, w x h
 * otherwise. Every entry is exactly 0 or +-MATHX_ONE and the translation a
 * whole number of pixels, so nothing rounds. */
static inline affine2x_t
affine2x_quarter_turn(int turn, int w, int h) {
    const int t = ((turn % 4) + 4) % 4;
    const int32_t fw = (int32_t)w << MATHX_SHIFT;
    const int32_t fh = (int32_t)h << MATHX_SHIFT;
    switch (t) {
        case 1: return (affine2x_t){{0, MATHX_ONE}, {-MATHX_ONE, 0}, {fw, 0}};
        case 2: return (affine2x_t){{-MATHX_ONE, 0}, {0, -MATHX_ONE}, {fw, fh}};
        case 3: return (affine2x_t){{0, -MATHX_ONE}, {MATHX_ONE, 0}, {0, fh}};
        default: return affine2x_identity();
    }
}

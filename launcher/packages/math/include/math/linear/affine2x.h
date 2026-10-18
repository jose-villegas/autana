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

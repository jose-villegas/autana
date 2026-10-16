/*
 * affine2f: a 2D affine map of a float, with integer-pixel maps.
 * The operations are affine2_template.h's; docs/math/README.md says how they are made.
 */
#pragma once

#include "math/geometry/rectf.h"
#include "math/geometry/recti.h"
#include "math/linear/affine2_template.h"
#include "math/linear/vec2f.h"
#include "math/linear/vec2i.h"
#include "math/scalar/mathf_round.h"

MATH_DEFINE_AFFINE2(affine2f, vec2f, rectf, float, mathf)

/* pixel = round(L * p + origin), nearest with ties away from zero. */
static inline vec2i_t
affine2f_map_pixel(affine2f_t t, vec2i_t p) {
    const vec2f_t mapped = affine2f_map_point(t, (vec2f_t){(float)p.x, (float)p.y});
    return (vec2i_t){mathf_round_i32(mapped.x), mathf_round_i32(mapped.y)};
}

MATH_DEFINE_AFFINE2_RECT(affine2f, vec2i, recti, mathi, map_recti, map_pixel)

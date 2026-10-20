/* box3f: a closed axis-aligned box of a float. */
#pragma once
#include <math.h>
#include "math/geometry/box3_template.h"
#include "math/geometry/box3s.h"
#include "math/geometry/planef.h"
#include "math/linear/vec_convert.h"

MATH_DEFINE_BOX3(box3f, vec3f, float, mathf, -INFINITY, INFINITY)
MATH_DEFINE_BOX3_REAL(box3f, vec3f, planef, float, mathf)

/* Each corner is converted with one tick = scale units. */
static inline box3f_t
box3f_from_box3s(box3s_t b, float scale) {
    return (box3f_t){vec3f_from_vec3s(b.lo, scale), vec3f_from_vec3s(b.hi, scale)};
}

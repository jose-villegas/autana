/* spheref: a closed sphere of a float. */
#pragma once
#include "math/geometry/box3f.h"
#include "math/geometry/sphere3s.h"
#include "math/geometry/sphere_template.h"

MATH_DEFINE_SPHERE(spheref, vec3f, box3f, float, mathf)

/* The centre is converted with one tick = scale units, and so is the radius. */
static inline spheref_t
spheref_from_sphere3s(sphere3s_t s, float scale) {
    return (spheref_t){vec3f_from_vec3s(s.centre, scale), (float)s.radius * scale};
}

/* spheref: a closed sphere of a float. */
#pragma once
#include "math/geometry/box3f.h"
#include "math/geometry/sphere_template.h"
#include "math/geometry/spheres.h"
#include "math/linear/vec_convert.h"

MATH_DEFINE_SPHERE(spheref, vec3f, vec3f, MATH_WIDEN_NONE, box3f, float, mathf)

/* The centre is converted with one tick = `units`, and so is the radius. */
static inline spheref_t
spheref_from_spheres(spheres_t s, float units) {
    return (spheref_t){vec3f_from_vec3s(s.centre, units), (float)s.radius * units};
}

/* quatf: float rotations constructed from directions. */
#include "math/linear/quatf_direction.h"

/* Prescale directions before squaring to avoid overflow and underflow.
 * A zero or nonfinite direction gives zero. */
static vec3f_t
rescale_direction(vec3f_t v) {
    if (!isfinite(v.x) || !isfinite(v.y) || !isfinite(v.z)) {
        return vec3f_zero();
    }
    const float largest = fmaxf(fabsf(v.x), fmaxf(fabsf(v.y), fabsf(v.z)));
    return largest > 0.0F ? (vec3f_t){v.x / largest, v.y / largest, v.z / largest} : vec3f_zero();
}

static double
dot_wide(vec3f_t a, vec3f_t b) {
    return (double)a.x * b.x + (double)a.y * b.y + (double)a.z * b.z;
}

/* Float inputs have exact products in double and their cross length fits
 * even at the extremes. Rounding the products in float can erase the axis. */
static double
cross_direction(vec3f_t a, vec3f_t b, vec3f_t* axis) {
    const double x = (double)a.y * b.z - (double)a.z * b.y;
    const double y = (double)a.z * b.x - (double)a.x * b.z;
    const double z = (double)a.x * b.y - (double)a.y * b.x;
    const double length = sqrt(x * x + y * y + z * z);
    *axis = vec3f_zero();
    if (isfinite(length) && length > 0.0) {
        *axis = (vec3f_t){(float)(x / length), (float)(y / length), (float)(z / length)};
    }
    return length;
}

quatf_t
quatf_looking(vec3f_t forward, vec3f_t up) {
    const vec3f_t scaled = rescale_direction(forward);
    if (vec3f_equal(scaled, vec3f_zero())) {
        return quatf_identity();
    }
    vec3f_t right;
    const double cross_length = cross_direction(up, forward, &right);
    if (!isfinite(cross_length) || cross_length == 0.0) {
        const vec3f_t substitute = forward.z > 0.0F ? vec3f_back() : vec3f_forward();
        if (cross_direction(substitute, forward, &right) == 0.0) {
            cross_direction(vec3f_up(), forward, &right);
        }
    }
    const vec3f_t front = vec3f_normalize(scaled);
    const vec3f_t above = vec3f_normalize(vec3f_cross(front, right));
    return quatf_normalize(quatf_from_basis(vec3f_cross(above, front), above, front));
}

quatf_t
quatf_from_to(vec3f_t a, vec3f_t b) {
    const double lengths = sqrt(dot_wide(a, a) * dot_wide(b, b));
    if (!isfinite(lengths) || lengths == 0.0) {
        return quatf_identity();
    }
    const double cosine = dot_wide(a, b) / lengths;
    vec3f_t axis;
    const double cross_length = cross_direction(a, b, &axis);
    const float sine = (float)(cross_length / lengths);
    if (cosine >= 0.0) {
        const vec3f_t imaginary = vec3f_scale(axis, sine);
        return quatf_normalize((quatf_t){imaginary.x, imaginary.y, imaginary.z, (float)(1.0 + cosine)});
    }
    if (cross_length == 0.0) {
        const vec3f_t reference = a.x == 0.0F && a.y == 0.0F ? vec3f_up() : vec3f_forward();
        cross_direction(reference, a, &axis);
        return (quatf_t){axis.x, axis.y, axis.z, 0.0F};
    }
    /* The complementary half-angle avoids cancellation as the turn approaches pi. */
    const vec3f_t imaginary = vec3f_scale(axis, (float)(1.0 - cosine));
    return quatf_normalize((quatf_t){imaginary.x, imaginary.y, imaginary.z, sine});
}

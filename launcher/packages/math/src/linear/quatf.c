/* quatf: float rotations constructed from directions. */
#include "math/linear/quatf_direction.h"

#include <float.h>

#define QUATF_OPPOSITION_SINE_TOLERANCE (8.0F * FLT_EPSILON)

/* Power-of-two scaling keeps the direction ratios through cross cancellation. */
static vec3f_t
rescale_direction(vec3f_t v) {
    if (!isfinite(v.x) || !isfinite(v.y) || !isfinite(v.z)) {
        return vec3f_zero();
    }
    const float largest = fmaxf(fabsf(v.x), fmaxf(fabsf(v.y), fabsf(v.z)));
    if (largest == 0.0F) {
        return vec3f_zero();
    }
    int exponent;
    frexpf(largest, &exponent);
    return (vec3f_t){scalbnf(v.x, -exponent), scalbnf(v.y, -exponent), scalbnf(v.z, -exponent)};
}

static vec3f_t
opposition_axis(vec3f_t a) {
    const float x = fabsf(a.x);
    const float y = fabsf(a.y);
    const float z = fabsf(a.z);
    if (z <= x && z <= y) {
        return vec3f_normalize(vec3f_cross(vec3f_unit_z(), a));
    }
    const vec3f_t basis = y <= x ? vec3f_unit_y() : vec3f_unit_x();
    return vec3f_normalize(vec3f_cross(basis, a));
}

quatf_t
quatf_looking(vec3f_t forward, vec3f_t up) {
    const vec3f_t scaled = rescale_direction(forward);
    if (vec3f_equal(scaled, vec3f_zero())) {
        return quatf_identity();
    }
    vec3f_t right = rescale_direction(vec3f_cross(rescale_direction(up), scaled));
    if (vec3f_equal(right, vec3f_zero())) {
        const vec3f_t substitute = forward.z > 0.0F ? vec3f_back() : vec3f_forward();
        right = rescale_direction(vec3f_cross(substitute, scaled));
        if (vec3f_equal(right, vec3f_zero())) {
            right = vec3f_cross(vec3f_up(), scaled);
        }
    }
    const vec3f_t front = vec3f_normalize(scaled);
    const vec3f_t above = vec3f_normalize(vec3f_cross(front, right));
    return quatf_normalize(quatf_from_basis(vec3f_cross(above, front), above, front));
}

quatf_t
quatf_from_to(vec3f_t a, vec3f_t b) {
    a = rescale_direction(a);
    b = rescale_direction(b);
    if (vec3f_equal(a, vec3f_zero()) || vec3f_equal(b, vec3f_zero())) {
        return quatf_identity();
    }
    const float a_squared = vec3f_dot(a, a);
    const float lengths = sqrtf(a_squared * vec3f_dot(b, b));
    const float dot = vec3f_dot(a, b);
    const vec3f_t cross = vec3f_cross(a, b);
    if (dot >= 0.0F) {
        return quatf_normalize((quatf_t){cross.x, cross.y, cross.z, lengths + dot});
    }
    const float cross_length = vec3f_length(cross);
    if (cross_length <= QUATF_OPPOSITION_SINE_TOLERANCE * lengths) {
        const vec3f_t axis = opposition_axis(a);
        return (quatf_t){axis.x, axis.y, axis.z, 0.0F};
    }
    /* Cancellation can tilt the computed axis toward a near opposition. */
    const vec3f_t orthogonal = vec3f_sub(cross, vec3f_scale(a, vec3f_dot(cross, a) / a_squared));
    const vec3f_t axis = vec3f_normalize(orthogonal);
    /* The complementary half-angle avoids cancellation as the turn approaches pi. */
    const vec3f_t imaginary = vec3f_scale(axis, lengths - dot);
    return quatf_normalize((quatf_t){imaginary.x, imaginary.y, imaginary.z, cross_length});
}

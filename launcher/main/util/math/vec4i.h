/*
 * vec4i: the integer homogeneous vector and its fixed-point unit: 512 to
 * 1.0, and 512 to a turn when the value is an angle. matrix4i.h's matrices
 * act on it. Header-only, static inline and ESP-IDF-free.
 */
#pragma once

#include <stdint.h>

#define VEC4I_SHIFT 9
#define VEC4I_ONE   (1 << VEC4I_SHIFT)

typedef int32_t vec4i_unit_t;

typedef struct {
    vec4i_unit_t x, y, z, w;
} vec4i_t;

static inline vec4i_unit_t
vec4i_unit_non_zero(vec4i_unit_t value) {
    return value + (value == 0);
}

static inline void
vec4i_init(vec4i_t* v) {
    *v = (vec4i_t){0, 0, 0, VEC4I_ONE};
}

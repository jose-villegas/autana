/* Host comparison operations preserve the measured linear-light variants. */
#pragma once
#include <assert.h>
#include "math/linear/vec3f.h"
#include "math/scalar/mathf_round.h"
#include "render/skin/r3d_skin_light_internal.h"
#include "skin_light_baseline.h"

/* light_normal records the normal transform of the measured host variant. */
static inline __attribute__((always_inline)) vec3f_t
light_normal(const mat4f_t* palette, const uint8_t* joints, const float* weights, int influences, vec3f_t normal) {
    vec3f_t rows[R3D_SKIN_NORMAL_WIDTH];
    for (int axis = 0; axis < R3D_SKIN_NORMAL_WIDTH; axis++) {
        const float* r = palette[joints[0]].m[axis];
        rows[axis] = vec3f_scale((vec3f_t){r[0], r[1], r[2]}, weights[0]);
        for (int i = 1; i < influences; i++) {
            r = palette[joints[i]].m[axis];
            rows[axis] = vec3f_add(rows[axis], vec3f_scale((vec3f_t){r[0], r[1], r[2]}, weights[i]));
        }
    }
    return (vec3f_t){vec3f_dot(rows[0], normal), vec3f_dot(rows[1], normal), vec3f_dot(rows[2], normal)};
}

static inline __attribute__((always_inline)) uint8_t
light_byte(float value) {
    value = value * R3D_SKIN_LIGHT_BYTE_SCALE + 0.5F;
    return (uint8_t)mathf_clamp_finite(value, 0.0F, R3D_SKIN_LIGHT_BYTE_SCALE);
}

static inline __attribute__((always_inline)) void
baseline_light_evaluate(vec3f_t normal, const r3d_skin_lights_t* lights, uint8_t out[3]) {
    const vec3f_t sum = light_irradiance(normal, lights);
    out[0] = light_byte(sum.x);
    out[1] = light_byte(sum.y);
    out[2] = light_byte(sum.z);
}

static inline __attribute__((always_inline)) int
axis_cell(int size, float u, unsigned* weight) {
    const float centres = 0.5F * (float)size;
    const float x = mathf_clamp_finite(u * centres + centres - 0.5F, 0.0F, (float)(size - 1));
    const int cell = (int)x < size - 2 ? (int)x : size - 2;
    *weight = (unsigned)((x - (float)cell) * SKIN_LIGHT_BASELINE_WEIGHT_ONE);
    return cell;
}

static inline __attribute__((always_inline)) void
light_bilinear(int size, const uint8_t (*rgb)[SKIN_LIGHT_BASELINE_CELL_BYTES], vec3f_t normal, uint8_t out[3]) {
    const vec2f_t p = vec3f_octahedral(normal);
    unsigned wx, wy;
    const int x = axis_cell(size, p.x, &wx);
    const int y = axis_cell(size, p.y, &wy);
    const uint8_t* a = rgb[y * size + x];
    const uint8_t* b = rgb[y * size + x + 1];
    const uint8_t* c = rgb[(y + 1) * size + x];
    const uint8_t* d = rgb[(y + 1) * size + x + 1];
    const unsigned rounding = 1U << (2 * SKIN_LIGHT_BASELINE_WEIGHT_BITS - 1);
    for (int k = 0; k < R3D_SKIN_LIGHT_CHANNELS; k++) {
        const unsigned top = a[k] * (SKIN_LIGHT_BASELINE_WEIGHT_ONE - wx) + b[k] * wx;
        const unsigned bottom = c[k] * (SKIN_LIGHT_BASELINE_WEIGHT_ONE - wx) + d[k] * wx;
        out[k] = (uint8_t)((top * (SKIN_LIGHT_BASELINE_WEIGHT_ONE - wy) + bottom * wy + rounding)
                           >> (2 * SKIN_LIGHT_BASELINE_WEIGHT_BITS));
    }
}

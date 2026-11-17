/* Transfer tables avoid powers and divisions in the vertex loop. */
#pragma once
#include "math/scalar/float_bits.h"
#include "math/scalar/mathf.h"
#include "render/skin/r3d_skin_light.h"

static inline __attribute__((always_inline)) vec3f_t
light_renormalise(vec3f_t normal) {
    const float length_squared = vec3f_dot(normal, normal);
    return length_squared > 0.0F ? vec3f_scale(normal, mathf_rsqrt_approx(length_squared)) : (vec3f_t){0};
}

static inline __attribute__((always_inline)) uint8_t
light_encode(float radiance, const r3d_skin_light_transfer_t* transfer) {
    if (!(radiance > 0.0F)) {
        return 0;
    }
    const uint32_t bits = mathf_bits(radiance);
    const int index = bits < R3D_SKIN_LIGHT_RADIANCE_SPLIT
                          ? (int)(bits >> R3D_SKIN_LIGHT_RADIANCE_LOW_SHIFT) - R3D_SKIN_LIGHT_RADIANCE_LOW_FIRST
                          : (int)(bits >> R3D_SKIN_LIGHT_RADIANCE_SHIFT) - R3D_SKIN_LIGHT_RADIANCE_FIRST
                                + R3D_SKIN_LIGHT_RADIANCE_LOW_SIZE;
    if (index < 0) {
        return 0;
    }
    return transfer->encode[index < R3D_SKIN_LIGHT_RADIANCE_SIZE ? index : R3D_SKIN_LIGHT_RADIANCE_SIZE - 1];
}

static inline __attribute__((always_inline)) float
light_decode(uint8_t albedo, const r3d_skin_light_transfer_t* transfer) {
    const uint32_t bits = (uint32_t)transfer->decode[albedo] << R3D_SKIN_LIGHT_DECODE_SHIFT;
    return mathf_from_bits(bits);
}

static inline __attribute__((always_inline)) vec3f_t
light_irradiance(vec3f_t normal, const r3d_skin_lights_t* lights) {
    vec3f_t sum = lights->ambient;
    for (int i = 0; i < lights->count; i++) {
        const float facing = vec3f_dot(normal, lights->direction[i]);
        if (facing > 0.0F) {
            sum = vec3f_add(sum, vec3f_scale(lights->color[i], facing));
        }
    }
    return sum;
}

static inline __attribute__((always_inline)) void
light_shade(vec3f_t sum, const r3d_skin_lights_t* lights, const uint8_t albedo[3], uint8_t out[3]) {
    const r3d_skin_light_transfer_t* transfer = lights->transfer;
    out[0] = light_encode(light_decode(albedo[0], transfer) * sum.x, transfer);
    out[1] = light_encode(light_decode(albedo[1], transfer) * sum.y, transfer);
    out[2] = light_encode(light_decode(albedo[2], transfer) * sum.z, transfer);
}

static inline __attribute__((always_inline)) void
light_evaluate(vec3f_t normal, const r3d_skin_lights_t* lights, const uint8_t albedo[3], uint8_t out[3]) {
    light_shade(light_irradiance(normal, lights), lights, albedo, out);
}

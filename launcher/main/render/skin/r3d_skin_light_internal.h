/* Transfer tables avoid powers and divisions in the vertex loop. */
#pragma once
#include <string.h>
#include "math/scalar/mathf_rsqrt_approx.h"
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
    uint32_t bits;
    memcpy(&bits, &radiance, sizeof bits);
    const int index = (int)(bits >> R3D_SKIN_LIGHT_RADIANCE_SHIFT) - R3D_SKIN_LIGHT_RADIANCE_FIRST;
    if (index < 0) {
        return 0;
    }
    if (index >= R3D_SKIN_LIGHT_RADIANCE_SIZE - 1) {
        return transfer->encode[R3D_SKIN_LIGHT_RADIANCE_SIZE - 1];
    }
    const unsigned weight = (bits >> (R3D_SKIN_LIGHT_RADIANCE_SHIFT - R3D_SKIN_LIGHT_TRANSFER_WEIGHT_BITS))
                            & (R3D_SKIN_LIGHT_TRANSFER_WEIGHT_ONE - 1U);
    const unsigned a = transfer->encode[index];
    const unsigned b = transfer->encode[index + 1];
    return (uint8_t)((a * (R3D_SKIN_LIGHT_TRANSFER_WEIGHT_ONE - weight) + b * weight
                      + R3D_SKIN_LIGHT_TRANSFER_WEIGHT_ONE / 2U)
                     >> R3D_SKIN_LIGHT_TRANSFER_WEIGHT_BITS);
}

static inline __attribute__((always_inline)) float
light_decode(uint8_t albedo, const r3d_skin_light_transfer_t* transfer) {
    const uint32_t bits = (uint32_t)transfer->decode[albedo] << R3D_SKIN_LIGHT_DECODE_SHIFT;
    float linear;
    memcpy(&linear, &bits, sizeof linear);
    return linear;
}

static inline __attribute__((always_inline)) vec3f_t
light_irradiance(vec3f_t normal, const r3d_skin_lights_t* lights) {
    vec3f_t sum = lights->ambient;
    for (int i = 0; i < lights->count; i++) {
        const float facing = vec3f_dot(normal, lights->direction[i]);
        if (facing > 0.0F) {
            sum = vec3f_add(sum, vec3f_scale(lights->colour[i], facing));
        }
    }
    return sum;
}

static inline __attribute__((always_inline)) void
light_evaluate(vec3f_t normal, const r3d_skin_lights_t* lights, const uint8_t albedo[3], uint8_t out[3]) {
    const vec3f_t sum = light_irradiance(normal, lights);
    const float channels[] = {sum.x, sum.y, sum.z};
    for (int k = 0; k < R3D_SKIN_LIGHT_CHANNELS; k++) {
        const float decoded = light_decode(albedo[k], lights->transfer);
        out[k] = light_encode(decoded * channels[k], lights->transfer);
    }
}

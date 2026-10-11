/* Transfer tables avoid powers and divisions in the vertex loop. */
#pragma once
#include <string.h>
#include "math/scalar/mathf_rsqrt.h"
#include "render/skin/r3d_skin_light.h"

/* Xtensa bit transfers avoid spilling each channel through the stack. */
static inline __attribute__((always_inline)) uint32_t
light_float_bits(float value) {
#ifdef __XTENSA__
    uint32_t bits;
    __asm__("rfr %0, %1" : "=a"(bits) : "f"(value));
    return bits;
#else
    uint32_t bits;
    memcpy(&bits, &value, sizeof bits);
    return bits;
#endif
}

static inline __attribute__((always_inline)) float
light_bits_float(uint32_t bits) {
#ifdef __XTENSA__
    float value;
    __asm__("wfr %0, %1" : "=f"(value) : "a"(bits));
    return value;
#else
    float value;
    memcpy(&value, &bits, sizeof value);
    return value;
#endif
}

static inline __attribute__((always_inline)) vec3f_t
light_renormalise(vec3f_t normal) {
    const float length_squared = vec3f_dot(normal, normal);
    return length_squared > 0.0F ? vec3f_scale(normal, mathf_rsqrt(length_squared)) : (vec3f_t){0};
}

static inline __attribute__((always_inline)) uint8_t
light_encode(float radiance, const r3d_skin_light_transfer_t* transfer) {
    if (!(radiance > 0.0F)) {
        return 0;
    }
    const uint32_t bits = light_float_bits(radiance);
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
    return light_bits_float(bits);
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
light_evaluate(vec3f_t normal, const r3d_skin_lights_t* lights, const uint8_t albedo[3], uint8_t out[3]) {
    const vec3f_t sum = light_irradiance(normal, lights);
    const r3d_skin_light_transfer_t* transfer = lights->transfer;
    out[0] = light_encode(light_decode(albedo[0], transfer) * sum.x, transfer);
    out[1] = light_encode(light_decode(albedo[1], transfer) * sum.y, transfer);
    out[2] = light_encode(light_decode(albedo[2], transfer) * sum.z, transfer);
}

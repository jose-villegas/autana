/* Transfer tables avoid powers and divisions in the vertex loop. */
#pragma once
#include "math/scalar/float_bits.h"
#include "math/scalar/mathf.h"
#include "render/skin/r3d_skin_light.h"

static inline bool
light_has_gradient(const sh1_irradiance_t* ambient) {
    for (int row = 0; row < SH1_CHANNELS; row++) {
        for (int column = 0; column < SH1_CHANNELS; column++) {
            if (ambient->gradient.m[row][column] != 0.0F) {
                return true;
            }
        }
    }
    return false;
}

static inline __attribute__((always_inline)) vec3f_t
light_renormalise(vec3f_t normal) {
    const float length_squared = vec3f_dot(normal, normal);
    return length_squared > 0.0F ? vec3f_scale(normal, mathf_rsqrt_approx(length_squared)) : vec3f_zero();
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
light_direct_irradiance(vec3f_t normal, const r3d_skin_lights_t* lights, vec3f_t sum) {
    for (int i = 0; i < lights->count; i++) {
        const float facing = vec3f_dot(normal, lights->direction[i]);
        if (facing > 0.0F) {
            sum = vec3f_add(sum, vec3f_scale(lights->color[i], facing));
        }
    }
    return sum;
}

static inline __attribute__((always_inline)) vec3f_t
light_irradiance(vec3f_t normal, const r3d_skin_lights_t* lights) {
    return light_direct_irradiance(normal, lights, lights->ambient);
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

static inline __attribute__((always_inline)) vec3f_t
light_table_sample(vec3f_t normal, const r3d_skin_lights_t* lights) {
    if (normal.x == 0.0F && normal.y == 0.0F && normal.z == 0.0F) {
        return lights->ambient;
    }
    const vec2f_t point = vec3f_octahedral(normal);
    const float half = 0.5F * (R3D_SKIN_LIGHT_TABLE_SIDE - 1);
    const float u = (point.x * half) + half;
    const float v = (point.y * half) + half;
    const int x = (int)u < R3D_SKIN_LIGHT_TABLE_SIDE - 1 ? (int)u : R3D_SKIN_LIGHT_TABLE_SIDE - 2;
    const int y = (int)v < R3D_SKIN_LIGHT_TABLE_SIDE - 1 ? (int)v : R3D_SKIN_LIGHT_TABLE_SIDE - 2;
    const int wx = (int)(((u - (float)x) * R3D_SKIN_LIGHT_TABLE_WEIGHT_ONE) + 0.5F);
    const int wy = (int)(((v - (float)y) * R3D_SKIN_LIGHT_TABLE_WEIGHT_ONE) + 0.5F);
    const uint32_t* cells = lights->table->cells + (y * R3D_SKIN_LIGHT_TABLE_SIDE) + x;
    const float scale = lights->table_scale / (R3D_SKIN_LIGHT_TABLE_WEIGHT_ONE * R3D_SKIN_LIGHT_TABLE_WEIGHT_ONE);
    float channels[R3D_SKIN_LIGHT_CHANNELS];
#pragma GCC unroll R3D_SKIN_LIGHT_CHANNELS
    for (int k = 0; k < R3D_SKIN_LIGHT_CHANNELS; k++) {
        const int shift = k * R3D_SKIN_LIGHT_TABLE_BITS;
        const int a = (int)((cells[0] >> shift) & R3D_SKIN_LIGHT_TABLE_MASK);
        const int b = (int)((cells[1] >> shift) & R3D_SKIN_LIGHT_TABLE_MASK);
        const int c = (int)((cells[R3D_SKIN_LIGHT_TABLE_SIDE] >> shift) & R3D_SKIN_LIGHT_TABLE_MASK);
        const int d = (int)((cells[R3D_SKIN_LIGHT_TABLE_SIDE + 1] >> shift) & R3D_SKIN_LIGHT_TABLE_MASK);
        const int top = (a * R3D_SKIN_LIGHT_TABLE_WEIGHT_ONE) + (wx * (b - a));
        const int bottom = (c * R3D_SKIN_LIGHT_TABLE_WEIGHT_ONE) + (wx * (d - c));
        channels[k] = (float)((top * R3D_SKIN_LIGHT_TABLE_WEIGHT_ONE) + (wy * (bottom - top))) * scale;
    }
    return (vec3f_t){channels[0], channels[1], channels[2]};
}

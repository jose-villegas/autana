/* Per-channel Reinhard scales the denominator by white; zero is identity. Gamma matches tools/r3d/light.py. */
#include <assert.h>
#include <math.h>
#include <string.h>
#include "render/skin/r3d_skin_light_directions.h"
#include "render/skin/r3d_skin_light_internal.h"

void
r3d_skin_light_transfer_build(float tonemap_white, r3d_skin_light_transfer_t* out) {
    assert(isfinite(tonemap_white) && tonemap_white >= 0.0F);
    for (int i = 0; i < R3D_SKIN_LIGHT_DECODE_SIZE; i++) {
        const float linear = powf((float)i / R3D_SKIN_LIGHT_BYTE_SCALE, R3D_SKIN_LIGHT_GAMMA);
        const uint32_t bits = light_float_bits(linear);
        out->decode[i] = (uint16_t)((bits + (1U << (R3D_SKIN_LIGHT_DECODE_SHIFT - 1))) >> R3D_SKIN_LIGHT_DECODE_SHIFT);
    }
    for (int i = 0; i < R3D_SKIN_LIGHT_RADIANCE_SIZE; i++) {
        const bool low = i < R3D_SKIN_LIGHT_RADIANCE_LOW_SIZE;
        const int shift = low ? R3D_SKIN_LIGHT_RADIANCE_LOW_SHIFT : R3D_SKIN_LIGHT_RADIANCE_SHIFT;
        const int bin = low ? i + R3D_SKIN_LIGHT_RADIANCE_LOW_FIRST
                            : i - R3D_SKIN_LIGHT_RADIANCE_LOW_SIZE + R3D_SKIN_LIGHT_RADIANCE_FIRST;
        const uint32_t bits = ((uint32_t)bin << shift) + (1U << (shift - 1));
        const float linear = light_bits_float(bits);
        const float mapped = linear / (1.0F + linear * tonemap_white);
        out->encode[i] =
            (uint8_t)lrintf(R3D_SKIN_LIGHT_BYTE_SCALE * powf(fminf(mapped, 1.0F), 1.0F / R3D_SKIN_LIGHT_GAMMA));
    }
}

float
r3d_skin_light_table_build(const r3d_skin_lights_t* lights, r3d_skin_light_table_t* out) {
    float maximum = 0.0F;
    for (int cell = 0; cell < R3D_SKIN_LIGHT_TABLE_CELLS; cell++) {
        const vec3f_t sum = light_irradiance(light_table_directions[cell], lights);
        if (sum.x > maximum) {
            maximum = sum.x;
        }
        if (sum.y > maximum) {
            maximum = sum.y;
        }
        if (sum.z > maximum) {
            maximum = sum.z;
        }
    }
    const float scale = maximum > 0.0F ? maximum / R3D_SKIN_LIGHT_TABLE_MASK : 1.0F;
    const float encode = maximum > 0.0F ? R3D_SKIN_LIGHT_TABLE_MASK / maximum : 0.0F;
    for (int cell = 0; cell < R3D_SKIN_LIGHT_TABLE_CELLS; cell++) {
        const vec3f_t sum = light_irradiance(light_table_directions[cell], lights);
        const float channels[] = {sum.x, sum.y, sum.z};
        uint32_t packed = 0;
#pragma GCC unroll R3D_SKIN_LIGHT_CHANNELS
        for (int k = 0; k < R3D_SKIN_LIGHT_CHANNELS; k++) {
            const uint32_t value = (uint32_t)(channels[k] * encode + 0.5F);
            packed |= value << (k * R3D_SKIN_LIGHT_TABLE_BITS);
        }
        out->cells[cell] = packed;
    }
    return scale;
}

vec3f_t
r3d_skin_light_table_sample(vec3f_t normal, const r3d_skin_lights_t* lights) {
    if (normal.x == 0.0F && normal.y == 0.0F && normal.z == 0.0F) {
        return lights->ambient;
    }
    const vec2f_t point = vec3f_octahedral(normal);
    const float half = 0.5F * (R3D_SKIN_LIGHT_TABLE_SIDE - 1);
    const float u = point.x * half + half, v = point.y * half + half;
    const int x = (int)u < R3D_SKIN_LIGHT_TABLE_SIDE - 1 ? (int)u : R3D_SKIN_LIGHT_TABLE_SIDE - 2;
    const int y = (int)v < R3D_SKIN_LIGHT_TABLE_SIDE - 1 ? (int)v : R3D_SKIN_LIGHT_TABLE_SIDE - 2;
    const int wx = (int)((u - x) * R3D_SKIN_LIGHT_TABLE_WEIGHT_ONE + 0.5F);
    const int wy = (int)((v - y) * R3D_SKIN_LIGHT_TABLE_WEIGHT_ONE + 0.5F);
    const uint32_t* cells = lights->table->cells + y * R3D_SKIN_LIGHT_TABLE_SIDE + x;
    const float scale = lights->table_scale / (R3D_SKIN_LIGHT_TABLE_WEIGHT_ONE * R3D_SKIN_LIGHT_TABLE_WEIGHT_ONE);
    float channels[R3D_SKIN_LIGHT_CHANNELS];
#pragma GCC unroll R3D_SKIN_LIGHT_CHANNELS
    for (int k = 0; k < R3D_SKIN_LIGHT_CHANNELS; k++) {
        const int shift = k * R3D_SKIN_LIGHT_TABLE_BITS;
        const int a = (cells[0] >> shift) & R3D_SKIN_LIGHT_TABLE_MASK;
        const int b = (cells[1] >> shift) & R3D_SKIN_LIGHT_TABLE_MASK;
        const int c = (cells[R3D_SKIN_LIGHT_TABLE_SIDE] >> shift) & R3D_SKIN_LIGHT_TABLE_MASK;
        const int d = (cells[R3D_SKIN_LIGHT_TABLE_SIDE + 1] >> shift) & R3D_SKIN_LIGHT_TABLE_MASK;
        const int top = a * R3D_SKIN_LIGHT_TABLE_WEIGHT_ONE + wx * (b - a);
        const int bottom = c * R3D_SKIN_LIGHT_TABLE_WEIGHT_ONE + wx * (d - c);
        channels[k] = (top * R3D_SKIN_LIGHT_TABLE_WEIGHT_ONE + wy * (bottom - top)) * scale;
    }
    return (vec3f_t){channels[0], channels[1], channels[2]};
}

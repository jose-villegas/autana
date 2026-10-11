/* Per-channel Reinhard scales the denominator by white; zero is identity. Gamma matches tools/r3d/light.py. */
#include <assert.h>
#include <math.h>
#include <string.h>
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
    vec3f_t ceiling = lights->ambient;
    for (int i = 0; i < lights->count; i++) {
        ceiling = vec3f_add(ceiling, lights->color[i]);
    }
    const float maximum = fmaxf(ceiling.x, fmaxf(ceiling.y, ceiling.z));
    const float scale = maximum > 0 ? maximum / R3D_SKIN_LIGHT_TABLE_MASK : 1.0F;
    const float encode = 1.0F / scale;
    for (int y = 0; y < R3D_SKIN_LIGHT_TABLE_SIDE; y++) {
        for (int x = 0; x < R3D_SKIN_LIGHT_TABLE_SIDE; x++) {
            const vec2f_t point = {2.0F * x / (R3D_SKIN_LIGHT_TABLE_SIDE - 1) - 1.0F,
                                   2.0F * y / (R3D_SKIN_LIGHT_TABLE_SIDE - 1) - 1.0F};
            const vec3f_t sum = light_irradiance(vec3f_from_octahedral(point), lights);
            const float channels[] = {sum.x, sum.y, sum.z};
            uint32_t cell = 0;
            for (int k = 0; k < R3D_SKIN_LIGHT_CHANNELS; k++) {
                const uint32_t value = (uint32_t)lrintf(fminf(channels[k] * encode, R3D_SKIN_LIGHT_TABLE_MASK));
                cell |= value << (k * R3D_SKIN_LIGHT_TABLE_BITS);
            }
            out->cells[y * R3D_SKIN_LIGHT_TABLE_SIDE + x] = cell;
        }
    }
    return scale;
}

vec3f_t
r3d_skin_light_table_sample(vec3f_t normal, const r3d_skin_lights_t* lights) {
    if (!(vec3f_dot(normal, normal) > 0.0F)) {
        return lights->ambient;
    }
    const vec2f_t point = vec3f_octahedral(normal);
    const float half = 0.5F * (R3D_SKIN_LIGHT_TABLE_SIDE - 1);
    const float u = mathf_clamp(point.x * half + half, 0, R3D_SKIN_LIGHT_TABLE_SIDE - 1);
    const float v = mathf_clamp(point.y * half + half, 0, R3D_SKIN_LIGHT_TABLE_SIDE - 1);
    const int x = (int)fminf(u, R3D_SKIN_LIGHT_TABLE_SIDE - 2);
    const int y = (int)fminf(v, R3D_SKIN_LIGHT_TABLE_SIDE - 2);
    const float wx = u - x, wy = v - y;
    const uint32_t* cells = lights->table->cells + y * R3D_SKIN_LIGHT_TABLE_SIDE + x;
    float channels[R3D_SKIN_LIGHT_CHANNELS];
    for (int k = 0; k < R3D_SKIN_LIGHT_CHANNELS; k++) {
        const int shift = k * R3D_SKIN_LIGHT_TABLE_BITS;
        const float a = (cells[0] >> shift) & R3D_SKIN_LIGHT_TABLE_MASK;
        const float b = (cells[1] >> shift) & R3D_SKIN_LIGHT_TABLE_MASK;
        const float c = (cells[R3D_SKIN_LIGHT_TABLE_SIDE] >> shift) & R3D_SKIN_LIGHT_TABLE_MASK;
        const float d = (cells[R3D_SKIN_LIGHT_TABLE_SIDE + 1] >> shift) & R3D_SKIN_LIGHT_TABLE_MASK;
        const float top = a + wx * (b - a), bottom = c + wx * (d - c);
        channels[k] = (top + wy * (bottom - top)) * lights->table_scale;
    }
    return (vec3f_t){channels[0], channels[1], channels[2]};
}

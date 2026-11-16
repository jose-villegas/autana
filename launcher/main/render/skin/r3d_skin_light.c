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
        const uint32_t bits = mathf_bits(linear);
        out->decode[i] = (uint16_t)((bits + (1U << (R3D_SKIN_LIGHT_DECODE_SHIFT - 1))) >> R3D_SKIN_LIGHT_DECODE_SHIFT);
    }
    for (int i = 0; i < R3D_SKIN_LIGHT_RADIANCE_SIZE; i++) {
        const bool low = i < R3D_SKIN_LIGHT_RADIANCE_LOW_SIZE;
        const int shift = low ? R3D_SKIN_LIGHT_RADIANCE_LOW_SHIFT : R3D_SKIN_LIGHT_RADIANCE_SHIFT;
        const int bin = low ? i + R3D_SKIN_LIGHT_RADIANCE_LOW_FIRST
                            : (i - R3D_SKIN_LIGHT_RADIANCE_LOW_SIZE) + R3D_SKIN_LIGHT_RADIANCE_FIRST;
        const uint32_t bits = ((uint32_t)bin << shift) + (1U << (shift - 1));
        const float linear = mathf_from_bits(bits);
        const float mapped = linear / (1.0F + (linear * tonemap_white));
        out->encode[i] =
            (uint8_t)lrintf(R3D_SKIN_LIGHT_BYTE_SCALE * powf(fminf(mapped, 1.0F), 1.0F / R3D_SKIN_LIGHT_GAMMA));
    }
}

/* Normal-indexed illumination (Miller and Hoffman 1984), octahedral directions (Cigolle et al. 2014). */
float
r3d_skin_light_table_build(const r3d_skin_lights_t* lights, r3d_skin_light_table_t* out) {
    /* Light-change builds use two passes to avoid a large scratch buffer and an allocation failure path. */
    float maximum = 0.0F;
    for (int cell = 0; cell < R3D_SKIN_LIGHT_TABLE_CELLS; cell++) {
        const vec3f_t sum = light_direct_irradiance(light_table_directions[cell], lights, lights->ambient);
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
    if (maximum < R3D_SKIN_LIGHT_IRRADIANCE_MIN) {
        memset(out, 0, sizeof *out);
        return 1.0F;
    }
    maximum = fminf(maximum, R3D_SKIN_LIGHT_IRRADIANCE_MAX);
    const float scale = maximum / R3D_SKIN_LIGHT_TABLE_MASK;
    const float encode = R3D_SKIN_LIGHT_TABLE_MASK / maximum;
    for (int cell = 0; cell < R3D_SKIN_LIGHT_TABLE_CELLS; cell++) {
        const vec3f_t sum = light_direct_irradiance(light_table_directions[cell], lights, lights->ambient);
        const float channels[] = {sum.x, sum.y, sum.z};
        uint32_t packed = 0;
#pragma GCC unroll R3D_SKIN_LIGHT_CHANNELS
        for (int k = 0; k < R3D_SKIN_LIGHT_CHANNELS; k++) {
            const uint32_t value = (uint32_t)(((channels[k] < maximum ? channels[k] : maximum) * encode) + 0.5F);
            packed |= value << (k * R3D_SKIN_LIGHT_TABLE_BITS);
        }
        out->cells[cell] = packed;
    }
    return scale;
}

vec3f_t
r3d_skin_light_table_sample(vec3f_t normal, const r3d_skin_lights_t* lights) {
    return light_table_sample(normal, lights);
}

uint8_t
r3d_skin_light_encode(float radiance, const r3d_skin_light_transfer_t* transfer) {
    return light_encode(radiance, transfer);
}

sh1_irradiance_t
r3d_skin_light_ambient_sh(const sh1_rgb_t* radiance) {
    sh1_irradiance_t ambient = sh1_rgb_irradiance(radiance);
    ambient.constant = vec3f_scale(ambient.constant, R3D_SKIN_LIGHT_LAMBERT_SCALE);
    ambient.gradient = mat3f_scale(ambient.gradient, R3D_SKIN_LIGHT_LAMBERT_SCALE);
    return ambient;
}

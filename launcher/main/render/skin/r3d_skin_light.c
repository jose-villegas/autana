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

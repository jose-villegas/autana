/* srgb: the sRGB transfer function of IEC 61966-2-1, between linear light and encoded [0, 1]. */
#pragma once

#include <math.h>
#include <stdint.h>

#define SRGB_ENCODED_CUTOFF 0.04045F
#define SRGB_LINEAR_CUTOFF  0.0031308F
#define SRGB_SLOPE          12.92F
#define SRGB_OFFSET         0.055F
#define SRGB_GAMMA          2.4F
#define SRGB_BYTE_MAX       255.0F

static inline float
srgbf_decode(float encoded) {
    return encoded <= SRGB_ENCODED_CUTOFF ? encoded / SRGB_SLOPE
                                          : powf((encoded + SRGB_OFFSET) / (1.0F + SRGB_OFFSET), SRGB_GAMMA);
}

/* Clamped to [0, 1] first; NaN encodes to 0. */
static inline float
srgbf_encode(float linear) {
    if (!(linear > 0.0F)) {
        return 0.0F;
    }
    if (linear >= 1.0F) {
        return 1.0F;
    }
    return linear <= SRGB_LINEAR_CUTOFF ? linear * SRGB_SLOPE
                                        : ((1.0F + SRGB_OFFSET) * powf(linear, 1.0F / SRGB_GAMMA)) - SRGB_OFFSET;
}

static inline float
srgbf_decode_byte(uint8_t encoded) {
    return srgbf_decode((float)encoded / SRGB_BYTE_MAX);
}

static inline uint8_t
srgbf_encode_byte(float linear) {
    return (uint8_t)((srgbf_encode(linear) * SRGB_BYTE_MAX) + 0.5F);
}

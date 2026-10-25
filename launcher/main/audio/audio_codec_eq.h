/* audio_codec_eq: ES8311 signed Q2.28 coefficients, most significant byte first. */
#pragma once
#include <math.h>
#include <stdint.h>
#define AUDIO_CODEC_EQ_ADC_HPF_REG     0x1B
#define AUDIO_CODEC_EQ_ADC_CONTROL_REG 0x1C
#define AUDIO_CODEC_EQ_ADC_BYPASS      0x40u
#define AUDIO_CODEC_EQ_ADC_B0_REG      0x1D
#define AUDIO_CODEC_EQ_ADC_A1_REG      0x21
#define AUDIO_CODEC_EQ_ADC_A2_REG      0x25
#define AUDIO_CODEC_EQ_ADC_B1_REG      0x29
#define AUDIO_CODEC_EQ_ADC_B2_REG      0x2D
#define AUDIO_CODEC_EQ_DAC_CONTROL_REG 0x37
#define AUDIO_CODEC_EQ_DAC_BYPASS      0x08u
#define AUDIO_CODEC_EQ_DAC_B0_REG      0x38
#define AUDIO_CODEC_EQ_DAC_B1_REG      0x3C
#define AUDIO_CODEC_EQ_DAC_A1_REG      0x40
#define AUDIO_CODEC_EQ_SCALE           268435456u
#define AUDIO_CODEC_EQ_MASK            0x3FFFFFFFu
#define AUDIO_CODEC_EQ_MIN             (-2.0)
#define AUDIO_CODEC_EQ_MAX             (2.0 - 1.0 / AUDIO_CODEC_EQ_SCALE)
#define AUDIO_CODEC_EQ_BYTES           4u
#define AUDIO_CODEC_EQ_BYTE_BITS       8u

static inline uint32_t
audio_codec_eq_encode(double value) {
    if (!isfinite(value)) {
        return 0;
    }
    if (value < AUDIO_CODEC_EQ_MIN) {
        value = AUDIO_CODEC_EQ_MIN;
    }
    if (value > AUDIO_CODEC_EQ_MAX) {
        value = AUDIO_CODEC_EQ_MAX;
    }
    return (uint32_t)(int64_t)llround(value * AUDIO_CODEC_EQ_SCALE) & AUDIO_CODEC_EQ_MASK;
}

static inline void
audio_codec_eq_bytes(uint32_t value, uint8_t out[AUDIO_CODEC_EQ_BYTES]) {
    value &= AUDIO_CODEC_EQ_MASK;
    for (unsigned i = 0; i < AUDIO_CODEC_EQ_BYTES; i++) {
        out[i] = (uint8_t)(value >> ((AUDIO_CODEC_EQ_BYTES - 1u - i) * AUDIO_CODEC_EQ_BYTE_BITS));
    }
}

/* ES8311 DAC limiter fields; level values are datasheet nibble indices. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define AUDIO_CODEC_DAC_VOLUME_REG   0x32
#define AUDIO_CODEC_DRC_ENABLE_REG   0x34
#define AUDIO_CODEC_DRC_LEVEL_REG    0x35
#define AUDIO_CODEC_DRC_RAMP_REG     0x37
#define AUDIO_CODEC_DRC_FIELD_MAX    15u
#define AUDIO_CODEC_DRC_ENABLE_SHIFT 7u
#define AUDIO_CODEC_DRC_ENABLE_MASK  (1u << AUDIO_CODEC_DRC_ENABLE_SHIFT)
#define AUDIO_CODEC_DRC_LEVEL_SHIFT  4u
#define AUDIO_CODEC_DRC_RAMP_SHIFT   4u

typedef struct {
    bool on;
    uint8_t winsize, max_level, min_level, ramp;
} audio_codec_drc_t;

static inline unsigned
audio_codec_drc_enable_reg(const audio_codec_drc_t* config) {
    return (config->on ? AUDIO_CODEC_DRC_ENABLE_MASK : 0u) | (config->winsize & AUDIO_CODEC_DRC_FIELD_MAX);
}

static inline unsigned
audio_codec_drc_level_reg(const audio_codec_drc_t* config) {
    return ((config->max_level & AUDIO_CODEC_DRC_FIELD_MAX) << AUDIO_CODEC_DRC_LEVEL_SHIFT)
           | (config->min_level & AUDIO_CODEC_DRC_FIELD_MAX);
}

static inline unsigned
audio_codec_drc_ramp_reg(unsigned old, const audio_codec_drc_t* config) {
    return (old & AUDIO_CODEC_DRC_FIELD_MAX)
           | ((config->ramp & AUDIO_CODEC_DRC_FIELD_MAX) << AUDIO_CODEC_DRC_RAMP_SHIFT);
}

bool audio_codec_drc_set(const audio_codec_drc_t* config);

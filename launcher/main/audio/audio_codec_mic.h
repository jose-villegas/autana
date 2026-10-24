/* audio_codec_mic: ES8311 ADC DSP fields and capture-scoped register transaction. */
#pragma once
#include "audio/audio_codec_feedback.h"

#define AUDIO_CODEC_MIC_VOLUME_REG      0x17
#define AUDIO_CODEC_MIC_ALC_REG         0x18
#define AUDIO_CODEC_MIC_LEVEL_REG       0x19
#define AUDIO_CODEC_MIC_GATE_REG        0x1A
#define AUDIO_CODEC_MIC_HPF1_REG        0x1B
#define AUDIO_CODEC_MIC_HPF2_REG        0x1C
#define AUDIO_CODEC_MIC_REGISTER_COUNT  (AUDIO_CODEC_MIC_HPF2_REG - AUDIO_CODEC_MIC_VOLUME_REG + 1)
#define AUDIO_CODEC_MIC_NIBBLE_MAX      15u
#define AUDIO_CODEC_MIC_HPF_MAX         31u
#define AUDIO_CODEC_MIC_DEPTH_MAX       7u
#define AUDIO_CODEC_MIC_GAIN_MAX        255u
#define AUDIO_CODEC_MIC_NIBBLE_SHIFT    4u
#define AUDIO_CODEC_MIC_DEPTH_SHIFT     5u
#define AUDIO_CODEC_MIC_ALC_ENABLE      (1u << 7u)
#define AUDIO_CODEC_MIC_GATE_ENABLE     (1u << 6u)
#define AUDIO_CODEC_MIC_EQ_BYPASS       (1u << 6u)
#define AUDIO_CODEC_MIC_HPF_DYNAMIC     (1u << 5u)
#define AUDIO_CODEC_MIC_VOLUME_DEFAULT  0xBF
#define AUDIO_CODEC_MIC_HPF_DEFAULT     10
#define AUDIO_CODEC_MIC_OFF_DEFAULT     0
#define AUDIO_CODEC_MIC_FIELD_DEFAULT   0
#define AUDIO_CODEC_MIC_DYNAMIC_DEFAULT 1
// clang-format off
#define AUDIO_CODEC_MIC_LEVEL_DB {-30.1F, -24.1F, -20.6F, -18.1F, -16.1F, -14.5F, -13.2F, -12.0F, -11.0F, -10.1F, -9.3F, -8.5F, -7.8F, -7.2F, -6.6F, -6.0F}
// clang-format on
#define AUDIO_CODEC_MIC_GATE_DB         {-96, -90, -84, -78, -72, -66, -60, -54, -51, -48, -45, -42, -39, -36, -33, -30}

typedef struct {
    bool alc, gate, hpf;
    uint8_t alc_max, alc_min, alc_win, alc_gain;
    uint8_t gate_ng, gate_win, gate_depth, hpf_s1, hpf_s2;
} audio_codec_mic_t;

#define AUDIO_CODEC_MIC_DEFAULT                                                                                        \
    {.alc = AUDIO_CODEC_MIC_OFF_DEFAULT,                                                                               \
     .gate = AUDIO_CODEC_MIC_OFF_DEFAULT,                                                                              \
     .hpf = AUDIO_CODEC_MIC_DYNAMIC_DEFAULT,                                                                           \
     .alc_max = AUDIO_CODEC_MIC_FIELD_DEFAULT,                                                                         \
     .alc_min = AUDIO_CODEC_MIC_FIELD_DEFAULT,                                                                         \
     .alc_win = AUDIO_CODEC_MIC_FIELD_DEFAULT,                                                                         \
     .alc_gain = AUDIO_CODEC_MIC_VOLUME_DEFAULT,                                                                       \
     .gate_ng = AUDIO_CODEC_MIC_FIELD_DEFAULT,                                                                         \
     .gate_win = AUDIO_CODEC_MIC_FIELD_DEFAULT,                                                                        \
     .gate_depth = AUDIO_CODEC_MIC_FIELD_DEFAULT,                                                                      \
     .hpf_s1 = AUDIO_CODEC_MIC_HPF_DEFAULT,                                                                            \
     .hpf_s2 = AUDIO_CODEC_MIC_HPF_DEFAULT}

static inline unsigned
audio_codec_mic_volume_reg(const audio_codec_mic_t* config) {
    return config->alc_gain;
}

static inline unsigned
audio_codec_mic_alc_reg(const audio_codec_mic_t* config) {
    return (config->alc ? AUDIO_CODEC_MIC_ALC_ENABLE : 0u) | (config->gate ? AUDIO_CODEC_MIC_GATE_ENABLE : 0u)
           | (config->alc_win & AUDIO_CODEC_MIC_NIBBLE_MAX);
}

static inline unsigned
audio_codec_mic_pair(unsigned high, unsigned low, unsigned shift) {
    return ((high << shift) | (low & ((1u << shift) - 1u))) & UINT8_MAX;
}

static inline unsigned
audio_codec_mic_level_reg(const audio_codec_mic_t* config) {
    return audio_codec_mic_pair(config->alc_max, config->alc_min, AUDIO_CODEC_MIC_NIBBLE_SHIFT);
}

static inline unsigned
audio_codec_mic_gate_reg(const audio_codec_mic_t* config) {
    return audio_codec_mic_pair(config->gate_win, config->gate_ng, AUDIO_CODEC_MIC_NIBBLE_SHIFT);
}

static inline unsigned
audio_codec_mic_hpf1_reg(const audio_codec_mic_t* config) {
    return audio_codec_mic_pair(config->gate_depth, config->hpf_s1, AUDIO_CODEC_MIC_DEPTH_SHIFT);
}

static inline unsigned
audio_codec_mic_hpf2_reg(const audio_codec_mic_t* config) {
    return AUDIO_CODEC_MIC_EQ_BYPASS | (config->hpf ? AUDIO_CODEC_MIC_HPF_DYNAMIC : 0u)
           | (config->hpf_s2 & AUDIO_CODEC_MIC_HPF_MAX);
}

typedef struct {
    int saved[AUDIO_CODEC_MIC_REGISTER_COUNT];
    bool restore_pending;
} audio_codec_mic_saved_t;

static inline bool
audio_codec_mic_restore_saved(audio_codec_mic_saved_t* state, void* context, audio_codec_set_reg_fn set_reg) {
    if (!state->restore_pending) {
        return true;
    }
    bool ok = true;
    for (unsigned i = 0; i < AUDIO_CODEC_MIC_REGISTER_COUNT; i++) {
        if (set_reg(context, AUDIO_CODEC_MIC_VOLUME_REG + (int)i, state->saved[i]) != 0) {
            ok = false;
        }
    }
    state->restore_pending = !ok;
    return ok;
}

static inline bool
audio_codec_mic_apply(audio_codec_mic_saved_t* state, void* context, audio_codec_get_reg_fn get_reg,
                      audio_codec_set_reg_fn set_reg, const audio_codec_mic_t* config) {
    if (!state->restore_pending) {
        for (unsigned i = 0; i < AUDIO_CODEC_MIC_REGISTER_COUNT; i++) {
            if (get_reg(context, AUDIO_CODEC_MIC_VOLUME_REG + (int)i, &state->saved[i]) != 0) {
                return false;
            }
        }
    }
    const unsigned values[AUDIO_CODEC_MIC_REGISTER_COUNT] = {
        audio_codec_mic_volume_reg(config), audio_codec_mic_alc_reg(config),  audio_codec_mic_level_reg(config),
        audio_codec_mic_gate_reg(config),   audio_codec_mic_hpf1_reg(config), audio_codec_mic_hpf2_reg(config)};
    /* A failed write may still reach the device. Retain every original until all restores succeed. */
    state->restore_pending = true;
    for (unsigned i = 0; i < AUDIO_CODEC_MIC_REGISTER_COUNT; i++) {
        if (set_reg(context, AUDIO_CODEC_MIC_VOLUME_REG + (int)i, (int)values[i]) != 0) {
            return false;
        }
    }
    return true;
}

bool audio_codec_mic_set(const audio_codec_mic_t* config);
bool audio_codec_mic_restore(void);

/* ES8311 DAC feedback routing preserves the original register value. */
#pragma once
#include <stdint.h>
#include "audio/audio_codec_saved.h"

#define AUDIO_CODEC_FEEDBACK_REG            0x44
#define AUDIO_CODEC_FEEDBACK_REGISTER_COUNT 1u
#define AUDIO_CODEC_ADCDAT_SHIFT            4u
#define AUDIO_CODEC_ADCDAT_MASK             (7u << AUDIO_CODEC_ADCDAT_SHIFT)
#define AUDIO_CODEC_ADCDAT_DAC_BOTH         6u

typedef struct {
    audio_codec_saved_t saved;
    bool enabled;
} audio_codec_feedback_t;

static inline unsigned
audio_codec_feedback_register(unsigned old) {
    return (old & ~AUDIO_CODEC_ADCDAT_MASK) | (AUDIO_CODEC_ADCDAT_DAC_BOTH << AUDIO_CODEC_ADCDAT_SHIFT);
}

static inline bool
audio_codec_feedback_set(audio_codec_feedback_t* state, void* context, audio_codec_get_reg_fn get_reg,
                         audio_codec_set_reg_fn set_reg, bool on) {
    if (!on) {
        if (state->saved.restore_pending) {
            state->enabled = false;
        }
        return audio_codec_saved_restore(&state->saved, context, set_reg);
    }
    if (state->saved.restore_pending) {
        return state->enabled;
    }
    if (!audio_codec_saved_capture(&state->saved, context, get_reg, AUDIO_CODEC_FEEDBACK_REG,
                                   AUDIO_CODEC_FEEDBACK_REGISTER_COUNT)) {
        return false;
    }
    const unsigned value = audio_codec_feedback_register((unsigned)state->saved.saved[0]);
    state->enabled = set_reg(context, AUDIO_CODEC_FEEDBACK_REG, (int)value) == 0;
    return state->enabled;
}

bool audio_codec_dac_feedback(bool on);

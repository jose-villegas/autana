/* Codec register transaction retaining the exact read-back value until restoration succeeds. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define AUDIO_CODEC_FEEDBACK_REG    0x44
#define AUDIO_CODEC_ADCDAT_SHIFT    4u
#define AUDIO_CODEC_ADCDAT_MASK     (7u << AUDIO_CODEC_ADCDAT_SHIFT)
#define AUDIO_CODEC_ADCDAT_DAC_BOTH 6u

typedef struct {
    int saved;
    bool restore_pending;
    bool enabled;
} audio_codec_feedback_t;

typedef int (*audio_codec_get_reg_fn)(void* context, int reg, int* value);
typedef int (*audio_codec_set_reg_fn)(void* context, int reg, int value);

static inline unsigned
audio_codec_feedback_register(unsigned old) {
    return (old & ~AUDIO_CODEC_ADCDAT_MASK) | (AUDIO_CODEC_ADCDAT_DAC_BOTH << AUDIO_CODEC_ADCDAT_SHIFT);
}

static inline bool
audio_codec_feedback_set(audio_codec_feedback_t* state, void* context, audio_codec_get_reg_fn get_reg,
                         audio_codec_set_reg_fn set_reg, bool on) {
    if (on) {
        if (state->restore_pending) {
            return state->enabled;
        }
        if (get_reg(context, AUDIO_CODEC_FEEDBACK_REG, &state->saved) != 0) {
            return false;
        }
        /* A failed write may reach the device; the caller must still restore its read-back. */
        state->restore_pending = true;
        state->enabled =
            set_reg(context, AUDIO_CODEC_FEEDBACK_REG, (int)audio_codec_feedback_register((unsigned)state->saved)) == 0;
        return state->enabled;
    }
    if (!state->restore_pending) {
        return true;
    }
    if (set_reg(context, AUDIO_CODEC_FEEDBACK_REG, state->saved) != 0) {
        /* A failed restore leaves the route unknown. */
        state->enabled = false;
        return false;
    }
    state->restore_pending = false;
    state->enabled = false;
    return true;
}

bool audio_codec_dac_feedback(bool on);

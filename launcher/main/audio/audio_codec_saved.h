/* Codec register ranges retain their read-backs until every restore write succeeds. */
#pragma once
#include <stdbool.h>

#define AUDIO_CODEC_MIC_VOLUME_REG     0x17
#define AUDIO_CODEC_MIC_ALC_REG        0x18
#define AUDIO_CODEC_MIC_LEVEL_REG      0x19
#define AUDIO_CODEC_MIC_GATE_REG       0x1A
#define AUDIO_CODEC_MIC_HPF1_REG       0x1B
#define AUDIO_CODEC_MIC_HPF2_REG       0x1C
#define AUDIO_CODEC_MIC_REGISTER_COUNT (AUDIO_CODEC_MIC_HPF2_REG - AUDIO_CODEC_MIC_VOLUME_REG + 1)
#define AUDIO_CODEC_SAVED_CAPACITY     AUDIO_CODEC_MIC_REGISTER_COUNT

typedef int (*audio_codec_get_reg_fn)(void* context, int reg, int* value);
typedef int (*audio_codec_set_reg_fn)(void* context, int reg, int value);

typedef struct {
    int saved[AUDIO_CODEC_SAVED_CAPACITY];
    int first;
    unsigned count;
    bool restore_pending;
} audio_codec_saved_t;

static inline bool
audio_codec_saved_capture(audio_codec_saved_t* state, void* context, audio_codec_get_reg_fn get_reg, int first,
                          unsigned count) {
    if (state->restore_pending) {
        return state->first == first && state->count == count;
    }
    for (unsigned i = 0; i < count; i++) {
        if (get_reg(context, first + (int)i, &state->saved[i]) != 0) {
            return false;
        }
    }
    state->first = first;
    state->count = count;
    /* A failed write may reach the device; retain the read-backs before any write. */
    state->restore_pending = true;
    return true;
}

static inline bool
audio_codec_saved_restore(audio_codec_saved_t* state, void* context, audio_codec_set_reg_fn set_reg) {
    bool ok = true;
    for (unsigned i = 0; state->restore_pending && i < state->count; i++) {
        ok &= set_reg(context, state->first + (int)i, state->saved[i]) == 0;
    }
    state->restore_pending = !ok;
    return ok;
}

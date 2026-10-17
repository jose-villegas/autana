/* Continuous phase, mono signed 16-bit PCM synthesis. */
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "audio/audio_out.h"
#include "math/scalar/trig.h"

#define AUDIO_TONE_PHASE_SHIFT (32 - TRIG_TURN_SHIFT)
#define AUDIO_TONE_TURN        ((uint64_t)TRIG_TURN << AUDIO_TONE_PHASE_SHIFT)

typedef struct {
    uint32_t phase;
    uint32_t step;
} audio_tone_t;

/* Step is rounded to the nearest 1/2^32 turn per sample. */
void audio_tone_init(audio_tone_t* state, uint16_t hz);
/* volume_pct must be <= AUDIO_OUT_VOLUME_MAX_PCT. */
void audio_tone_fill(audio_tone_t* state, int16_t* out, size_t n, uint8_t volume_pct);

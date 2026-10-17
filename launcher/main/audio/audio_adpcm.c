#include "audio/audio_adpcm.h"

#include <limits.h>
#include "math/scalar/mathi.h"

static const int16_t STEP[] = AUDIO_ADPCM_STEP_TABLE;
static const int8_t INDEX[] = AUDIO_ADPCM_INDEX_TABLE;

enum { PENDING_CODE = 1U << AUDIO_ADPCM_CODE_BITS };

static int16_t
step(audio_adpcm_t* state, unsigned code) {
    const int magnitude = STEP[state->step_index];
    int diff = magnitude >> 3;
    if (code & 1U) {
        diff += magnitude >> 2;
    }
    if (code & 2U) {
        diff += magnitude >> 1;
    }
    if (code & 4U) {
        diff += magnitude;
    }
    state->predictor =
        (int16_t)mathi_clamp(state->predictor + (code & AUDIO_ADPCM_SIGN_BIT ? -diff : diff), INT16_MIN, INT16_MAX);
    state->step_index = (int8_t)mathi_clamp(state->step_index + INDEX[code], 0, AUDIO_ADPCM_STEP_INDEX_MAX);
    return state->predictor;
}

void
audio_adpcm_read(audio_adpcm_t* state, int16_t* out, uint32_t frames) {
    if (frames != 0 && state->pending != 0) {
        *out++ = step(state, state->pending & AUDIO_ADPCM_CODE_MASK);
        state->pending = 0;
        frames--;
    }
    while (frames >= AUDIO_ADPCM_CODES_PER_BYTE) {
        const unsigned codes = *state->codes++;
        *out++ = step(state, codes & AUDIO_ADPCM_CODE_MASK);
        *out++ = step(state, codes >> AUDIO_ADPCM_CODE_BITS);
        frames -= AUDIO_ADPCM_CODES_PER_BYTE;
    }
    if (frames != 0) {
        const unsigned codes = *state->codes++;
        *out = step(state, codes & AUDIO_ADPCM_CODE_MASK);
        state->pending = (uint8_t)((codes >> AUDIO_ADPCM_CODE_BITS) | PENDING_CODE);
    }
}

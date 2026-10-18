#include "audio/audio_adpcm.h"

#include <limits.h>
#include "math/scalar/mathi.h"

static const int16_t STEP[] = AUDIO_ADPCM_STEP_TABLE;
static const int8_t INDEX[] = AUDIO_ADPCM_INDEX_TABLE;

enum { PENDING_CODE = 1U << AUDIO_ADPCM_CODE_BITS };

static inline __attribute__((always_inline)) int
step(int* predictor, int* step_index, unsigned code) {
    const int magnitude = STEP[*step_index];
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
    *predictor = mathi_clamp(*predictor + (code & AUDIO_ADPCM_SIGN_BIT ? -diff : diff), INT16_MIN, INT16_MAX);
    *step_index = mathi_clamp(*step_index + INDEX[code], 0, AUDIO_ADPCM_STEP_INDEX_MAX);
    return *predictor;
}

void
audio_adpcm_read(audio_adpcm_t* state, int16_t* out, uint32_t frames) {
    const uint8_t* codes = state->codes;
    int predictor = state->predictor;
    int step_index = state->step_index;
    unsigned pending = state->pending;
    if (frames != 0 && pending != 0) {
        *out++ = (int16_t)step(&predictor, &step_index, pending & AUDIO_ADPCM_CODE_MASK);
        pending = 0;
        frames--;
    }
    while (frames >= AUDIO_ADPCM_CODES_PER_BYTE) {
        const unsigned packed = *codes++;
        *out++ = (int16_t)step(&predictor, &step_index, packed & AUDIO_ADPCM_CODE_MASK);
        *out++ = (int16_t)step(&predictor, &step_index, packed >> AUDIO_ADPCM_CODE_BITS);
        frames -= AUDIO_ADPCM_CODES_PER_BYTE;
    }
    if (frames != 0) {
        const unsigned packed = *codes++;
        *out = (int16_t)step(&predictor, &step_index, packed & AUDIO_ADPCM_CODE_MASK);
        pending = (packed >> AUDIO_ADPCM_CODE_BITS) | PENDING_CODE;
    }
    state->codes = codes;
    state->predictor = (int16_t)predictor;
    state->step_index = (int8_t)step_index;
    state->pending = (uint8_t)pending;
}

/* The phase retains fractional table positions across every output block. */
#include "audio/audio_tone.h"

void
audio_tone_init(audio_tone_t* state, uint16_t hz) {
    *state = (audio_tone_t){
        .step = (uint32_t)(((uint64_t)hz * AUDIO_TONE_TURN + AUDIO_OUT_SAMPLE_RATE_HZ / 2) / AUDIO_OUT_SAMPLE_RATE_HZ)};
}

void
audio_tone_fill(audio_tone_t* state, int16_t* out, size_t n, uint8_t volume_pct) {
    for (size_t i = 0; i < n; i++) {
        const int32_t sample = trig_sin((uint16_t)(state->phase >> AUDIO_TONE_PHASE_SHIFT));
        out[i] = (int16_t)(sample * volume_pct / (int32_t)AUDIO_OUT_VOLUME_MAX_PCT);
        state->phase += state->step;
    }
}

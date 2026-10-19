/* Tone detection and phase-difference frequency measurement. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "audio/audio_codec.h"
#include "audio/audio_out.h"
#include "math/signal/goertzel.h"

/* Trim the first captured samples after audio starts. */
#define AUDIO_LOOPBACK_SETTLE_MS       100u
#define AUDIO_LOOPBACK_HEARD_MARGIN_DB 20.0F

static inline size_t
audio_loopback_sample_budget(uint32_t ms) {
    return audio_out_samples(AUDIO_LOOPBACK_SETTLE_MS) + audio_out_samples(ms);
}

/* Consecutive windows resolve nominal frequency +/- sample_rate / (2 * window_samples). */
#define AUDIO_LOOPBACK_WINDOW_SAMPLES        (2u * AUDIO_CODEC_BLOCK_SAMPLES)
#define AUDIO_LOOPBACK_MIN_FREQUENCY_SAMPLES (2u * AUDIO_LOOPBACK_WINDOW_SAMPLES)

typedef struct {
    goertzel_t bin;
    float nominal_hz;
    float previous_phase;
    double phase_error;
    size_t advances;
    bool has_phase;
} audio_loopback_frequency_t;

void audio_loopback_frequency_init(audio_loopback_frequency_t* state, uint16_t hz);
void audio_loopback_frequency_feed(audio_loopback_frequency_t* state, const int16_t* samples, size_t count);
float audio_loopback_frequency_hz(const audio_loopback_frequency_t* state);

/* Blocking; call from a worker or console task, never the frame loop. */
bool audio_loopback_measure(uint16_t hz, uint16_t ms, float* db, float* measured_hz);

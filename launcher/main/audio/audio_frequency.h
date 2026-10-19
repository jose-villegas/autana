/* Audio frequency estimation from consecutive single-bin phases, following Flanagan and Golden 1966. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "audio/audio_out.h"
#include "math/signal/goertzel.h"

/* Consecutive windows resolve nominal frequency +/- sample_rate / (2 * window_samples). */
#define AUDIO_LOOPBACK_WINDOW_SAMPLES        256u
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

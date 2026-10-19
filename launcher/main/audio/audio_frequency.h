/* Audio frequency estimation from consecutive single-bin phases, following Flanagan and Golden 1966. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "audio/audio_codec.h"
#include "audio/audio_out.h"
#include "math/signal/goertzel.h"

/* Consecutive windows resolve nominal frequency +/- sample_rate / (2 * window_samples). */
#define AUDIO_FREQUENCY_WINDOW_SAMPLES (2u * AUDIO_CODEC_BLOCK_SAMPLES)
#define AUDIO_FREQUENCY_MIN_SAMPLES    (2u * AUDIO_FREQUENCY_WINDOW_SAMPLES)
#define AUDIO_FREQUENCY_MIN_CYCLES     4u
/* A window spans N/Fs seconds, so C cycles require f >= C*Fs/N and f <= Fs/2 - C*Fs/N. */
#define AUDIO_FREQUENCY_EDGE_HZ                                                                                        \
    ((float)(AUDIO_FREQUENCY_MIN_CYCLES * AUDIO_OUT_SAMPLE_RATE_HZ) / AUDIO_FREQUENCY_WINDOW_SAMPLES)

typedef struct {
    goertzel_t bin;
    float nominal_hz;
    float previous_phase;
    double phase_error;
    size_t advances;
    bool has_phase;
} audio_frequency_t;

void audio_frequency_init(audio_frequency_t* state, uint16_t hz);
void audio_frequency_feed(audio_frequency_t* state, const int16_t* samples, size_t count);
float audio_frequency_hz(const audio_frequency_t* state);

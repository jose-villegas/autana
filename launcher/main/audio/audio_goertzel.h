/* Streaming generalized Goertzel evaluation, including non-integer DFT bins.
 * Method: docs/Citations.md#71. Power is squared unnormalized DFT magnitude;
 * dBFS normalizes a full-scale target sine to 0 dB. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#define AUDIO_GOERTZEL_FLOOR_DB (-120.0f)

typedef struct {
    float coefficient;
    float previous;
    float previous2;
    size_t samples;
} audio_goertzel_t;

void audio_goertzel_init(audio_goertzel_t* state, uint16_t hz, uint32_t sample_rate);
void audio_goertzel_feed(audio_goertzel_t* state, const int16_t* samples, size_t n);
float audio_goertzel_power(const audio_goertzel_t* state);
float audio_goertzel_dbfs(const audio_goertzel_t* state);

/* Streaming single-bin detection (Goertzel, 1958).
 * Power is the squared unnormalized DFT magnitude. */
#pragma once
#include <stddef.h>
#include <stdint.h>

#define GOERTZEL_LEVEL_FLOOR_DB (-120.0F)

typedef struct {
    float coefficient;
    float previous;
    float previous2;
    size_t samples;
} goertzel_t;

void goertzel_init(goertzel_t* state, float hz, float sample_rate_hz);
void goertzel_reset(goertzel_t* state);
void goertzel_feed_s(goertzel_t* state, const int16_t* samples, size_t n);
void goertzel_feed_f(goertzel_t* state, const float* samples, size_t n);
float goertzel_power(const goertzel_t* state);
float goertzel_amplitude(const goertzel_t* state);
float goertzel_level_db(const goertzel_t* state, float full_scale);

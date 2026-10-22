/* Streaming single-bin detection (Goertzel, 1958).
 * Power is the squared unnormalized DFT magnitude. */
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "math/scalar/math_db.h"

typedef struct {
    float coefficient;
    float sine;
    float omega;
    float previous;
    float previous2;
    size_t samples;
} goertzel_t;

/* 0 < hz < sample_rate_hz / 2 (DC cancels in float); hz need not fall on an integer DFT bin. */
void goertzel_init(goertzel_t* state, float hz, float sample_rate_hz);
/* Clears the stream, keeps the frequency. */
void goertzel_reset(goertzel_t* state);
/* Any split of a stream into blocks gives the same result. */
void goertzel_feed_s(goertzel_t* state, const int16_t* samples, size_t n);
void goertzel_feed_f(goertzel_t* state, const float* samples, size_t n);
float goertzel_power(const goertzel_t* state);
/* DFT e^{-j w n}, n=0 since init/reset: radians in (-pi, pi], zero before any sample. */
float goertzel_phase(const goertzel_t* state);
/* Amplitude of a sine at the bin; 0 before any sample. */
float goertzel_amplitude(const goertzel_t* state);
/* A sine of amplitude full_scale reads 0 dB; nothing reads below MATH_DB_FLOOR. */
float goertzel_level_db(const goertzel_t* state, float full_scale);

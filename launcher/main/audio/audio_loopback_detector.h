/* Streaming loopback metrics; THD excludes noise between harmonic bins. */
#pragma once
#include "audio/audio_frequency.h"
#include "audio/audio_loopback.h"

typedef struct {
    goertzel_t fundamental;
    goertzel_t harmonics[AUDIO_LOOPBACK_HARMONICS - AUDIO_LOOPBACK_FIRST_HARMONIC + 1U];
    audio_frequency_t frequency;
    size_t harmonic_count;
    int peak;
    uint64_t square_sum;
    size_t sample_count;
} audio_loopback_detector_t;

void audio_loopback_detector_init(audio_loopback_detector_t* detector, uint16_t hz, bool analyze);
void audio_loopback_detector_feed(audio_loopback_detector_t* detector, const int16_t* samples, size_t count,
                                  bool analyze);
void audio_loopback_detector_result(const audio_loopback_detector_t* detector, audio_loopback_result_t* result);

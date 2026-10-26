/* signal_figures_probe: real detector responses and equal-temperament pitches. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "math/scalar/math_const.h"
#include "math/signal/goertzel.h"
#include "math/signal/pitch.h"

static const int response_lengths[] = {64, 128, 256};
static const float sample_rate_hz = 8192.0F;
static const float detector_fraction = 0.125F;
static const int response_points = 241;
static const float response_span_bins = 3.0F;
static const int sweep_frames = 60;
static const int window_samples = 256;
static const float detector_hz[] = {640.0F, 1024.0F, 1408.0F};
static const float sweep_start_hz = 400.0F;
static const float sweep_end_hz = 1700.0F;
static const float reference_hz = 440.0F;
static const int pitch_octaves = 2;
static const int pitch_subdivisions = 4;

static void
response_samples(void) {
    printf("\"sample_rate_hz\":%.9g,\"response\":[", sample_rate_hz);
    for (size_t j = 0; j < sizeof response_lengths / sizeof response_lengths[0]; j++) {
        const int n = response_lengths[j];
        float* samples = calloc((size_t)n, sizeof *samples);
        if (!samples) {
            exit(EXIT_FAILURE);
        }
        goertzel_t detector;
        goertzel_init(&detector, sample_rate_hz * detector_fraction, sample_rate_hz);
        printf("%s{\"n\":%d,\"samples\":[", j ? "," : "", n);
        for (int point = 0; point < response_points; point++) {
            const float bins = response_span_bins * (2.0F * point / (response_points - 1) - 1);
            const float frequency = detector_fraction + bins / n;
            for (int i = 0; i < n; i++) {
                samples[i] = sinf(MATH_TAU * frequency * i);
            }
            goertzel_reset(&detector);
            goertzel_feed_f(&detector, samples, (size_t)n);
            printf("%s[%.9g,%.9g]", point ? "," : "", bins, goertzel_amplitude(&detector));
        }
        printf("]}");
        free(samples);
    }
    printf("]");
}

static void
sweep_samples(void) {
    float* samples = calloc((size_t)window_samples, sizeof *samples);
    if (!samples) {
        exit(EXIT_FAILURE);
    }
    const float duration = (float)(sweep_frames * window_samples) / sample_rate_hz;
    const float slope = (sweep_end_hz - sweep_start_hz) / duration;
    printf(",\"detectors\":[");
    for (size_t j = 0; j < sizeof detector_hz / sizeof detector_hz[0]; j++) {
        printf("%s%.9g", j ? "," : "", detector_hz[j]);
    }
    printf("],\"sweep\":[");
    for (int frame = 0; frame < sweep_frames; frame++) {
        const float middle_s = ((float)(frame * window_samples) + (float)window_samples / 2) / sample_rate_hz;
        printf("%s{\"time\":%.9g,\"hz\":%.9g", frame ? "," : "", middle_s, sweep_start_hz + slope * middle_s);
        for (int i = 0; i < window_samples; i++) {
            const float time = (float)(frame * window_samples + i) / sample_rate_hz;
            samples[i] = sinf(MATH_TAU * (sweep_start_hz * time + slope * time * time / 2));
        }
        printf(",\"amplitude\":[");
        for (size_t j = 0; j < sizeof detector_hz / sizeof detector_hz[0]; j++) {
            goertzel_t detector;
            goertzel_init(&detector, detector_hz[j], sample_rate_hz);
            goertzel_reset(&detector);
            goertzel_feed_f(&detector, samples, (size_t)window_samples);
            printf("%s%.9g", j ? "," : "", goertzel_amplitude(&detector));
        }
        printf("]}");
    }
    printf("]");
    free(samples);
}

int
main(void) {
    printf("{");
    response_samples();
    sweep_samples();
    printf(",\"octave\":%d,\"reference_hz\":%.9g,\"pitch\":[", PITCH_SEMITONES_PER_OCTAVE, reference_hz);
    const int bound = pitch_octaves * PITCH_SEMITONES_PER_OCTAVE * pitch_subdivisions;
    for (int i = -bound; i <= bound; i++) {
        const float semitones = (float)i / pitch_subdivisions;
        const float hz = pitch_semitone_hz(reference_hz, semitones);
        printf("%s[%.9g,%.9g]", i == -bound ? "" : ",", semitones, hz);
    }
    puts("]}");
    return 0;
}

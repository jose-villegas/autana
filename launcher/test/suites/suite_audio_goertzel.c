/* Detector compared with an independent double-precision DFT on the host. */
#include <math.h>
#include <stdlib.h>
#include "audio/audio_goertzel.h"
#include "audio/audio_out.h"
#include "math/scalar/trig.h"
#include "suites.h"
#include "test_cleanup.h"
#include "unity.h"
#define TEST_SAMPLES               AUDIO_OUT_SAMPLE_RATE_HZ
#define TEST_OFF_TONE_REJECTION_DB 30.0f
#define TEST_ODD_BLOCK             7u
#define TEST_TARGET_HZ             1000u
#define TEST_REJECT_HZ             3000u
static int16_t* buffer;

static void
cleanup(void) {
    free(buffer);
    buffer = NULL;
}

static int16_t*
fixture(void) {
    buffer = calloc(TEST_SAMPLES, sizeof *buffer);
    TEST_ASSERT_NOT_NULL(buffer);
    suite_set_test_cleanup(cleanup);
    return buffer;
}
#ifndef DEVICE_BUILD
#define TEST_PI           3.14159265358979323846
#define TEST_DB_TOLERANCE 0.5f

static void
test_goertzel_reference(void) {
    static const uint16_t frequencies[] = {300, TEST_TARGET_HZ, TEST_REJECT_HZ};
    static const double amplitudes[] = {1.0, 0.01};
    int16_t* samples = fixture();
    for (size_t f = 0; f < sizeof frequencies / sizeof frequencies[0]; f++) {
        for (size_t a = 0; a < sizeof amplitudes / sizeof amplitudes[0]; a++) {
            double re = 0, im = 0;
            for (size_t i = 0; i < TEST_SAMPLES; i++) {
                double phase = 2 * TEST_PI * frequencies[f] * i / AUDIO_OUT_SAMPLE_RATE_HZ;
                samples[i] = (int16_t)lround(TRIG_SIN_MAX * amplitudes[a] * sin(phase));
                re += samples[i] * cos(phase);
                im -= samples[i] * sin(phase);
            }
            audio_goertzel_t state;
            audio_goertzel_init(&state, frequencies[f], AUDIO_OUT_SAMPLE_RATE_HZ);
            audio_goertzel_feed(&state, samples, TEST_SAMPLES);
            double scale = (double)TEST_SAMPLES * TRIG_SIN_MAX / 2;
            float reference = (float)(10 * log10((re * re + im * im) / (scale * scale)));
            TEST_ASSERT_FLOAT_WITHIN(TEST_DB_TOLERANCE, reference, audio_goertzel_dbfs(&state));
            TEST_ASSERT_FLOAT_WITHIN(TEST_DB_TOLERANCE, (float)(20 * log10(amplitudes[a])),
                                     audio_goertzel_dbfs(&state));
        }
    }
}
#endif
static void
test_goertzel_streaming_silence_rejection(void) {
    int16_t* samples = fixture();
    audio_goertzel_t state;
    audio_goertzel_init(&state, TEST_TARGET_HZ, AUDIO_OUT_SAMPLE_RATE_HZ);
    audio_goertzel_feed(&state, samples, TEST_SAMPLES);
    TEST_ASSERT_EQUAL_FLOAT(AUDIO_GOERTZEL_FLOOR_DB, audio_goertzel_dbfs(&state));
    TEST_ASSERT_EQUAL_FLOAT(0, audio_goertzel_power(&state));
    for (size_t i = 0; i < TEST_SAMPLES; i++) {
        samples[i] = (int16_t)trig_sin((uint16_t)((uint64_t)i * TEST_TARGET_HZ * TRIG_TURN / AUDIO_OUT_SAMPLE_RATE_HZ));
    }
    audio_goertzel_init(&state, TEST_TARGET_HZ, AUDIO_OUT_SAMPLE_RATE_HZ);
    audio_goertzel_feed(&state, samples, TEST_SAMPLES);
    audio_goertzel_t split;
    audio_goertzel_init(&split, TEST_TARGET_HZ, AUDIO_OUT_SAMPLE_RATE_HZ);
    for (size_t i = 0; i < TEST_SAMPLES;) {
        size_t n = TEST_SAMPLES - i < TEST_ODD_BLOCK ? TEST_SAMPLES - i : TEST_ODD_BLOCK;
        audio_goertzel_feed(&split, samples + i, n);
        i += n;
    }
    TEST_ASSERT_EQUAL_FLOAT(audio_goertzel_power(&state), audio_goertzel_power(&split));
    audio_goertzel_t off;
    audio_goertzel_init(&off, TEST_REJECT_HZ, AUDIO_OUT_SAMPLE_RATE_HZ);
    audio_goertzel_feed(&off, samples, TEST_SAMPLES);
    TEST_ASSERT_TRUE(audio_goertzel_dbfs(&state) - audio_goertzel_dbfs(&off) >= TEST_OFF_TONE_REJECTION_DB);
}

void
run_audio_goertzel_suite(void) {
#ifndef DEVICE_BUILD
    RUN_TEST(test_goertzel_reference);
#endif
    RUN_TEST(test_goertzel_streaming_silence_rejection);
}
SUITE_REGISTER(run_audio_goertzel_suite)

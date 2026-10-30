/* Detector compared with an independent double-precision DFT on the host. */
#include <math.h>
#include <stdlib.h>
#include "math/scalar/math_const.h"
#include "math/scalar/trig.h"
#include "math/signal/goertzel.h"
#include "testkit/cleanup.h"
#include "testkit/suite.h"
#include "unity.h"
#define TEST_SAMPLE_RATE_HZ        22050u
#define TEST_SAMPLES               TEST_SAMPLE_RATE_HZ
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

/* An on-bin TEST_TARGET_HZ tone from the integer sine table, divided down by divisor. */
static void
fill_tone(int16_t* samples, int32_t divisor) {
    for (size_t i = 0; i < TEST_SAMPLES; i++) {
        samples[i] =
            (int16_t)(trig_sin((uint16_t)((uint64_t)i * TEST_TARGET_HZ * TRIG_TURN / TEST_SAMPLE_RATE_HZ)) / divisor);
    }
}
#ifndef ESP_PLATFORM
#define TEST_PI           3.14159265358979323846
#define TEST_DB_TOLERANCE 0.5f
#define TEST_MAX_RECORD_S 60u

static void
test_goertzel_reference(void) {
    static const uint16_t frequencies[] = {300, TEST_TARGET_HZ, TEST_REJECT_HZ};
    static const double amplitudes[] = {1.0, 0.01};
    int16_t* samples = fixture();
    for (size_t f = 0; f < sizeof frequencies / sizeof frequencies[0]; f++) {
        for (size_t a = 0; a < sizeof amplitudes / sizeof amplitudes[0]; a++) {
            double re = 0, im = 0;
            for (size_t i = 0; i < TEST_SAMPLES; i++) {
                double phase = 2 * TEST_PI * frequencies[f] * i / TEST_SAMPLE_RATE_HZ;
                samples[i] = (int16_t)lround(TRIG_SIN_MAX * amplitudes[a] * sin(phase));
                re += samples[i] * cos(phase);
                im -= samples[i] * sin(phase);
            }
            goertzel_t state;
            goertzel_init(&state, frequencies[f], TEST_SAMPLE_RATE_HZ);
            goertzel_feed_s(&state, samples, TEST_SAMPLES);
            double scale = (double)TEST_SAMPLES * TRIG_SIN_MAX / 2;
            float reference = math_db_power((float)((re * re + im * im) / (scale * scale)));
            TEST_ASSERT_FLOAT_WITHIN(TEST_DB_TOLERANCE, reference, goertzel_level_db(&state, TRIG_SIN_MAX));
            TEST_ASSERT_FLOAT_WITHIN(TEST_DB_TOLERANCE, math_db_amplitude(amplitudes[a]),
                                     goertzel_level_db(&state, TRIG_SIN_MAX));
        }
    }
}

static void
test_goertzel_noninteger_bins(void) {
    static const uint16_t frequencies[] = {300, 9000};
    static const size_t record_samples[] = {5225, 6615};
    static const uint16_t offsets[] = {0, 37};
    int16_t* samples = fixture();
    for (size_t f = 0; f < sizeof frequencies / sizeof frequencies[0]; f++) {
        for (size_t d = 0; d < sizeof record_samples / sizeof record_samples[0]; d++) {
            const size_t n = record_samples[d];
            for (size_t o = 0; o < sizeof offsets / sizeof offsets[0]; o++) {
                double re = 0, im = 0;
                for (size_t i = 0; i < n; i++) {
                    const double phase = 2 * TEST_PI * i / TEST_SAMPLE_RATE_HZ;
                    samples[i] = (int16_t)lround(TRIG_SIN_MAX * sin(phase * (frequencies[f] + offsets[o])));
                    re += samples[i] * cos(phase * frequencies[f]);
                    im -= samples[i] * sin(phase * frequencies[f]);
                }
                goertzel_t state;
                goertzel_init(&state, frequencies[f], TEST_SAMPLE_RATE_HZ);
                goertzel_feed_s(&state, samples, n);
                const double scale = (double)n * TRIG_SIN_MAX / 2;
                const float reference = math_db_power((float)((re * re + im * im) / (scale * scale)));
                TEST_ASSERT_FLOAT_WITHIN(TEST_DB_TOLERANCE, reference, goertzel_level_db(&state, TRIG_SIN_MAX));
            }
        }
    }
}

static void
test_goertzel_max_record(void) {
    int16_t* samples = fixture();
    goertzel_t state;
    goertzel_init(&state, TEST_TARGET_HZ, TEST_SAMPLE_RATE_HZ);
    const size_t total = (size_t)TEST_MAX_RECORD_S * TEST_SAMPLE_RATE_HZ;
    for (size_t offset = 0; offset < total;) {
        const size_t n = total - offset < TEST_SAMPLES ? total - offset : TEST_SAMPLES;
        for (size_t i = 0; i < n; i++) {
            const double phase = 2 * TEST_PI * TEST_TARGET_HZ * (offset + i) / TEST_SAMPLE_RATE_HZ;
            samples[i] = (int16_t)lround(TRIG_SIN_MAX * sin(phase));
        }
        goertzel_feed_s(&state, samples, n);
        offset += n;
    }
    TEST_ASSERT_EQUAL_UINT(total, state.samples);
    TEST_ASSERT_FLOAT_WITHIN(TEST_DB_TOLERANCE, 0, goertzel_level_db(&state, TRIG_SIN_MAX));
}
#endif
static void
test_goertzel_streaming_silence_rejection(void) {
    int16_t* samples = fixture();
    goertzel_t state;
    goertzel_init(&state, TEST_TARGET_HZ, TEST_SAMPLE_RATE_HZ);
    goertzel_feed_s(&state, samples, TEST_SAMPLES);
    TEST_ASSERT_EQUAL_FLOAT(MATH_DB_FLOOR, goertzel_level_db(&state, TRIG_SIN_MAX));
    TEST_ASSERT_EQUAL_FLOAT(0, goertzel_power(&state));
    fill_tone(samples, 1);
    goertzel_init(&state, TEST_TARGET_HZ, TEST_SAMPLE_RATE_HZ);
    goertzel_feed_s(&state, samples, TEST_SAMPLES);
    goertzel_t split;
    goertzel_init(&split, TEST_TARGET_HZ, TEST_SAMPLE_RATE_HZ);
    for (size_t i = 0; i < TEST_SAMPLES;) {
        size_t n = TEST_SAMPLES - i < TEST_ODD_BLOCK ? TEST_SAMPLES - i : TEST_ODD_BLOCK;
        goertzel_feed_s(&split, samples + i, n);
        i += n;
    }
    TEST_ASSERT_EQUAL_FLOAT(goertzel_power(&state), goertzel_power(&split));
    goertzel_t off;
    goertzel_init(&off, TEST_REJECT_HZ, TEST_SAMPLE_RATE_HZ);
    goertzel_feed_s(&off, samples, TEST_SAMPLES);
    TEST_ASSERT_TRUE(goertzel_level_db(&state, TRIG_SIN_MAX) - goertzel_level_db(&off, TRIG_SIN_MAX)
                     >= TEST_OFF_TONE_REJECTION_DB);
}

#define TEST_FLOAT_BLOCK         5u
#define TEST_FLOAT_AMPLITUDE     0.5F
#define TEST_FLOAT_TOLERANCE     0.001F
#define TEST_RESET_SAMPLES       3u
#define TEST_FULL_SCALE_FACTOR   4.0F
#define TEST_AMPLITUDE_TOLERANCE 4.0F
#define TEST_LEVEL_TOLERANCE     0.001F
#define TEST_AMPLITUDE_DIVISOR   2
#define TEST_AMPLITUDE           (TRIG_SIN_MAX / TEST_AMPLITUDE_DIVISOR)
#define TEST_TINY_FULL_SCALE     1.0e12F

static void
assert_same_state(const goertzel_t* expected, const goertzel_t* actual) {
    TEST_ASSERT_EQUAL_MEMORY(&expected->coefficient, &actual->coefficient, sizeof expected->coefficient);
    TEST_ASSERT_EQUAL_MEMORY(&expected->previous, &actual->previous, sizeof expected->previous);
    TEST_ASSERT_EQUAL_MEMORY(&expected->previous2, &actual->previous2, sizeof expected->previous2);
    TEST_ASSERT_EQUAL_UINT(expected->samples, actual->samples);
}

static void
test_goertzel_float_feed(void) {
    int16_t* samples = fixture();
    fill_tone(samples, 1);
    goertzel_t integers;
    goertzel_t floats;
    goertzel_init(&integers, TEST_TARGET_HZ, TEST_SAMPLE_RATE_HZ);
    goertzel_init(&floats, TEST_TARGET_HZ, TEST_SAMPLE_RATE_HZ);
    for (size_t i = 0; i < TEST_SAMPLES; i++) {
        const float sample = (float)samples[i];
        goertzel_feed_f(&floats, &sample, 1);
    }
    goertzel_feed_s(&integers, samples, TEST_SAMPLES);
    assert_same_state(&integers, &floats);
}

static void
test_goertzel_float_blocks(void) {
    float* samples = malloc(TEST_SAMPLES * sizeof *samples);
    TEST_ASSERT_NOT_NULL(samples);
    for (size_t i = 0; i < TEST_SAMPLES; i++) {
        samples[i] = TEST_FLOAT_AMPLITUDE
                     * sinf(MATH_TAU * (float)(i * TEST_TARGET_HZ % TEST_SAMPLE_RATE_HZ) / (float)TEST_SAMPLE_RATE_HZ);
    }
    goertzel_t single;
    goertzel_t blocks;
    goertzel_init(&single, TEST_TARGET_HZ, TEST_SAMPLE_RATE_HZ);
    goertzel_init(&blocks, TEST_TARGET_HZ, TEST_SAMPLE_RATE_HZ);
    for (size_t i = 0; i < TEST_SAMPLES; i++) {
        goertzel_feed_f(&single, samples + i, 1);
    }
    for (size_t i = 0; i < TEST_SAMPLES;) {
        const size_t n = TEST_SAMPLES - i < TEST_FLOAT_BLOCK ? TEST_SAMPLES - i : TEST_FLOAT_BLOCK;
        goertzel_feed_f(&blocks, samples + i, n);
        i += n;
    }
    free(samples);
    assert_same_state(&single, &blocks);
    /* A normalized fractional sine keeps its amplitude: nothing rounds float samples. */
    TEST_ASSERT_FLOAT_WITHIN(TEST_FLOAT_TOLERANCE, TEST_FLOAT_AMPLITUDE, goertzel_amplitude(&blocks));
}

static void
test_goertzel_reset(void) {
    static const int16_t samples[TEST_RESET_SAMPLES] = {TRIG_SIN_MAX, -TRIG_SIN_MAX, TRIG_SIN_MAX};
    goertzel_t state;
    goertzel_t fresh;
    goertzel_init(&state, TEST_TARGET_HZ, TEST_SAMPLE_RATE_HZ);
    goertzel_init(&fresh, TEST_TARGET_HZ, TEST_SAMPLE_RATE_HZ);
    TEST_ASSERT_EQUAL_FLOAT(0.0F, goertzel_amplitude(&state));
    TEST_ASSERT_EQUAL_FLOAT(MATH_DB_FLOOR, goertzel_level_db(&state, TRIG_SIN_MAX));
    goertzel_feed_s(&state, samples, TEST_RESET_SAMPLES);
    TEST_ASSERT_NOT_EQUAL(0.0F, state.previous2);
    goertzel_reset(&state);
    assert_same_state(&fresh, &state);
    goertzel_feed_s(&state, samples, TEST_RESET_SAMPLES);
    goertzel_feed_s(&fresh, samples, TEST_RESET_SAMPLES);
    assert_same_state(&fresh, &state);
}

static void
test_goertzel_amplitude_and_scale(void) {
    int16_t* samples = fixture();
    fill_tone(samples, TEST_AMPLITUDE_DIVISOR);
    goertzel_t state;
    goertzel_init(&state, TEST_TARGET_HZ, TEST_SAMPLE_RATE_HZ);
    goertzel_feed_s(&state, samples, TEST_SAMPLES);
    TEST_ASSERT_FLOAT_WITHIN(TEST_AMPLITUDE_TOLERANCE, TEST_AMPLITUDE, goertzel_amplitude(&state));
    const float level = goertzel_level_db(&state, TRIG_SIN_MAX);
    TEST_ASSERT_FLOAT_WITHIN(TEST_LEVEL_TOLERANCE, level - math_db_amplitude(TEST_FULL_SCALE_FACTOR),
                             goertzel_level_db(&state, TRIG_SIN_MAX * TEST_FULL_SCALE_FACTOR));
    /* Non-zero power far below the floor still reads the floor. */
    TEST_ASSERT_TRUE(goertzel_power(&state) > 0.0F);
    TEST_ASSERT_EQUAL_FLOAT(MATH_DB_FLOOR, goertzel_level_db(&state, TEST_TINY_FULL_SCALE));
}

#define TEST_PHASE_SAMPLES        441u
#define TEST_PHASE_LONG_SAMPLES   65537u
#define TEST_PHASE_LONG_TOLERANCE 0.03F
#define TEST_ROTATION_TOLERANCE   0.00001F
#define TEST_ROTATION_LARGE_COUNT 16777218u
#define TEST_PHASE_TOLERANCE      0.001F
#define TEST_SINE_PHASE_TOLERANCE 0.02F

#ifndef ESP_PLATFORM
static void
assert_phase_record(int16_t* samples, size_t count, float hz, float phase, float tolerance) {
    goertzel_t state, split;
    goertzel_init(&state, hz, TEST_SAMPLE_RATE_HZ);
    goertzel_init(&split, hz, TEST_SAMPLE_RATE_HZ);
    TEST_ASSERT_EQUAL_FLOAT(0, goertzel_phase(&state));
    double re = 0, im = 0;
    for (size_t i = 0; i < count; i++) {
        const double angle = (double)MATH_TAU * TEST_TARGET_HZ * i / TEST_SAMPLE_RATE_HZ;
        samples[i % TEST_SAMPLES] = (int16_t)lround(TRIG_SIN_MAX * cos(angle + (double)phase));
        const double bin_angle = (double)MATH_TAU * (double)hz * i / TEST_SAMPLE_RATE_HZ;
        re += samples[i % TEST_SAMPLES] * cos(bin_angle);
        im -= samples[i % TEST_SAMPLES] * sin(bin_angle);
        if ((i + 1) % TEST_SAMPLES == 0 || i + 1 == count) {
            const size_t n = i % TEST_SAMPLES + 1;
            goertzel_feed_s(&state, samples, n);
            goertzel_feed_s(&split, samples, TEST_ODD_BLOCK);
            goertzel_feed_s(&split, samples + TEST_ODD_BLOCK, n - TEST_ODD_BLOCK);
        }
    }
    TEST_ASSERT_FLOAT_WITHIN(tolerance, 0, remainderf(goertzel_phase(&state) - (float)atan2(im, re), MATH_TAU));
    TEST_ASSERT_EQUAL_FLOAT(goertzel_phase(&state), goertzel_phase(&split));
    if (hz == TEST_TARGET_HZ) {
        TEST_ASSERT_FLOAT_WITHIN(tolerance, 0, remainderf(goertzel_phase(&state) - phase, MATH_TAU));
    }
    goertzel_reset(&state);
    for (size_t i = 0; i < count; i++) {
        samples[i % TEST_SAMPLES] = (int16_t)lround(
            TRIG_SIN_MAX * sin((double)MATH_TAU * (double)hz * i / TEST_SAMPLE_RATE_HZ + (double)phase));
        if ((i + 1) % TEST_SAMPLES == 0 || i + 1 == count) {
            goertzel_feed_s(&state, samples, i % TEST_SAMPLES + 1);
        }
    }
    TEST_ASSERT_FLOAT_WITHIN(TEST_SINE_PHASE_TOLERANCE, 0,
                             remainderf(goertzel_phase(&state) - phase + MATH_PI / 2, MATH_TAU));
}

static void
test_goertzel_phase(void) {
    static const float phases[] = {-2.0F, 0.0F, 1.2F, MATH_PI - TEST_PHASE_TOLERANCE * 2,
                                   MATH_PI + TEST_PHASE_TOLERANCE * 2};
    static const float bins[] = {TEST_TARGET_HZ, TEST_TARGET_HZ + 3.7F};
    static const size_t counts[] = {TEST_PHASE_SAMPLES, TEST_PHASE_LONG_SAMPLES};
    int16_t* samples = fixture();
    for (size_t c = 0; c < sizeof counts / sizeof counts[0]; c++) {
        const size_t count = counts[c];
        const float tolerance = count == TEST_PHASE_SAMPLES ? TEST_PHASE_TOLERANCE : TEST_PHASE_LONG_TOLERANCE;
        for (size_t b = 0; b < sizeof bins / sizeof bins[0]; b++) {
            for (size_t p = 0; p < sizeof phases / sizeof phases[0]; p++) {
                assert_phase_record(samples, count, bins[b], phases[p], tolerance);
            }
        }
    }
}

static void
test_goertzel_phase_long_rotation(void) {
    static const size_t counts[] = {TEST_PHASE_LONG_SAMPLES, TEST_ROTATION_LARGE_COUNT, UINT32_MAX};
    static const float frequencies[] = {TEST_TARGET_HZ, TEST_SAMPLE_RATE_HZ / 2.0F - TEST_TARGET_HZ};
    for (size_t c = 0; c < sizeof counts / sizeof counts[0]; c++) {
        for (size_t f = 0; f < sizeof frequencies / sizeof frequencies[0]; f++) {
            goertzel_t state;
            goertzel_init(&state, frequencies[f], TEST_SAMPLE_RATE_HZ);
            state.samples = counts[c];
            state.previous = 1.0F;
            const float reference =
                (float)remainder(-(double)state.omega * (double)(state.samples - 1), (double)MATH_TAU);
            TEST_ASSERT_FLOAT_WITHIN(TEST_ROTATION_TOLERANCE, 0,
                                     remainderf(goertzel_phase(&state) - reference, MATH_TAU));
        }
    }
}

static void
test_goertzel_phase_negative_pi(void) {
    const goertzel_t state = {.omega = MATH_PI / 2, .previous = -1, .previous2 = -0.0F, .samples = 1};
    TEST_ASSERT_EQUAL_FLOAT(MATH_PI, goertzel_phase(&state));
}
#endif

void
run_goertzel_suite(void) {
#ifndef ESP_PLATFORM
    RUN_TEST(test_goertzel_phase);
    RUN_TEST(test_goertzel_phase_negative_pi);
    RUN_TEST(test_goertzel_phase_long_rotation);
    RUN_TEST(test_goertzel_reference);
    RUN_TEST(test_goertzel_noninteger_bins);
    RUN_TEST(test_goertzel_max_record);
#endif
    RUN_TEST(test_goertzel_streaming_silence_rejection);
    RUN_TEST(test_goertzel_float_feed);
    RUN_TEST(test_goertzel_float_blocks);
    RUN_TEST(test_goertzel_reset);
    RUN_TEST(test_goertzel_amplitude_and_scale);
}
SUITE_REGISTER(run_goertzel_suite)

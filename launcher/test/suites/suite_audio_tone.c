/* Portable synthesis checks against time, amplitude and chunking contracts. */
#include <math.h>
#include <stdlib.h>
#include "audio/audio_frequency.h"
#include "audio/audio_loopback.h"
#include "audio/audio_tone.h"
#include "math/scalar/math_const.h"
#include "math/scalar/rng.h"
#include "suites.h"
#include "test_cleanup.h"
#include "unity.h"
#define TEST_HZ          1000u
#define TEST_HALF_VOLUME (AUDIO_OUT_VOLUME_MAX_PCT / 2)
static int16_t* buffers[2];
static size_t allocated;

static void
cleanup(void) {
    while (allocated) {
        free(buffers[--allocated]);
    }
}

static int16_t*
fixture(size_t n) {
    int16_t* out = malloc(n * sizeof *out);
    TEST_ASSERT_NOT_NULL(out);
    suite_set_test_cleanup(cleanup);
    buffers[allocated++] = out;
    return out;
}

static void
test_tone_frequency(void) {
    static const uint16_t frequencies[] = {300, 1000, 3000};
    int16_t* out = fixture(AUDIO_OUT_SAMPLE_RATE_HZ);
    for (size_t f = 0; f < sizeof frequencies / sizeof frequencies[0]; f++) {
        audio_tone_t tone;
        audio_tone_init(&tone, frequencies[f]);
        audio_tone_fill(&tone, out, AUDIO_OUT_SAMPLE_RATE_HZ, AUDIO_OUT_VOLUME_MAX_PCT);
#ifndef DEVICE_BUILD
        const double turns = (double)frequencies[f];
        const double phase_turn = (double)tone.phase / ((double)UINT32_MAX + 1);
        const double error_hz = remainder(phase_turn - turns, 1.0);
        const double half_step_hz = 0.51 * AUDIO_OUT_SAMPLE_RATE_HZ / ((double)UINT32_MAX + 1);
        TEST_ASSERT_TRUE(fabs(error_hz) <= half_step_hz);
#endif
        unsigned crossings = 0;
        for (size_t i = 1; i < AUDIO_OUT_SAMPLE_RATE_HZ; i++) {
            if (out[i - 1] <= 0 && out[i] > 0) {
                crossings++;
            }
        }
        TEST_ASSERT_UINT_WITHIN(1, frequencies[f], crossings);
    }
}

static void
test_tone_chunks_and_volume(void) {
    static const size_t chunks[] = {1, 7, 256};
    const size_t n = AUDIO_OUT_SAMPLE_RATE_HZ;
    int16_t* whole = fixture(n);
    int16_t* split = fixture(n);
    audio_tone_t tone;
    audio_tone_init(&tone, TEST_HZ);
    audio_tone_fill(&tone, whole, n, AUDIO_OUT_VOLUME_MAX_PCT);
    for (size_t c = 0; c < sizeof chunks / sizeof chunks[0]; c++) {
        audio_tone_init(&tone, TEST_HZ);
        for (size_t i = 0; i < n;) {
            size_t count = n - i < chunks[c] ? n - i : chunks[c];
            audio_tone_fill(&tone, split + i, count, AUDIO_OUT_VOLUME_MAX_PCT);
            i += count;
        }
        TEST_ASSERT_EQUAL_INT16_ARRAY(whole, split, n);
    }
    audio_tone_init(&tone, TEST_HZ);
    audio_tone_fill(&tone, split, n, TEST_HALF_VOLUME);
    for (size_t i = 0; i < n; i++) {
        TEST_ASSERT_INT_WITHIN(1, whole[i] / 2, split[i]);
        TEST_ASSERT_TRUE(abs(whole[i]) <= TRIG_SIN_MAX);
    }
    tone.phase = (uint32_t)TRIG_QUARTER_TURN << AUDIO_TONE_PHASE_SHIFT;
    audio_tone_fill(&tone, split, 1, AUDIO_OUT_VOLUME_MAX_PCT);
    TEST_ASSERT_EQUAL_INT16(TRIG_SIN_MAX, split[0]);
    tone.phase = (uint32_t)(TRIG_TURN - TRIG_QUARTER_TURN) << AUDIO_TONE_PHASE_SHIFT;
    audio_tone_fill(&tone, split, 1, AUDIO_OUT_VOLUME_MAX_PCT);
    TEST_ASSERT_EQUAL_INT16(-TRIG_SIN_MAX, split[0]);
    audio_tone_fill(&tone, split, n, 0);
    for (size_t i = 0; i < n; i++) {
        TEST_ASSERT_EQUAL_INT16(0, split[i]);
    }
}

static void
test_loopback_measured_budget(void) {
    static const uint32_t durations[] = {1, AUDIO_LOOPBACK_SETTLE_MS - 1, AUDIO_LOOPBACK_SETTLE_MS,
                                         AUDIO_LOOPBACK_SETTLE_MS + 1};
    for (size_t i = 0; i < sizeof durations / sizeof durations[0]; i++) {
        const size_t budget = audio_loopback_sample_budget(durations[i]);
        const size_t settle = audio_out_samples(AUDIO_LOOPBACK_SETTLE_MS);
        TEST_ASSERT_TRUE(budget > settle);
        TEST_ASSERT_EQUAL_UINT(audio_out_samples(durations[i]), budget - settle);
    }
}

#define TEST_ESTIMATE_MS                 500u
#define TEST_ESTIMATE_AMPLITUDE          12000.0F
#define TEST_ESTIMATE_NOISE              0.01F
#define TEST_NOISE_RMS_MATCH_SCALE       2.449489743F
#define TEST_ESTIMATE_TOLERANCE_HZ       0.05F
#define TEST_ESTIMATE_NOISE_TOLERANCE_HZ 0.5F

static void
test_loopback_frequency_boundaries_and_blocks(void) {
    const size_t count = AUDIO_LOOPBACK_MIN_FREQUENCY_SAMPLES;
    int16_t* samples = fixture(count);
    audio_tone_t tone;
    audio_tone_init(&tone, TEST_HZ);
    audio_tone_fill(&tone, samples, count, TEST_HALF_VOLUME);
    audio_loopback_frequency_t single;
    audio_loopback_frequency_init(&single, TEST_HZ);
    for (size_t i = 0; i < count; i++) {
        audio_loopback_frequency_feed(&single, samples + i, 1);
        if (i + 1 < count) {
            TEST_ASSERT_TRUE(isnan(audio_loopback_frequency_hz(&single)));
        }
    }
    TEST_ASSERT_TRUE(isfinite(audio_loopback_frequency_hz(&single)));
    static const size_t chunks[] = {7, AUDIO_LOOPBACK_WINDOW_SAMPLES - 1, AUDIO_LOOPBACK_WINDOW_SAMPLES + 17};
    for (size_t c = 0; c < sizeof chunks / sizeof chunks[0]; c++) {
        audio_loopback_frequency_t blocks;
        audio_loopback_frequency_init(&blocks, TEST_HZ);
        for (size_t i = 0; i < count;) {
            const size_t n = count - i < chunks[c] ? count - i : chunks[c];
            audio_loopback_frequency_feed(&blocks, samples + i, n);
            i += n;
        }
        TEST_ASSERT_EQUAL_FLOAT(audio_loopback_frequency_hz(&single), audio_loopback_frequency_hz(&blocks));
        TEST_ASSERT_EQUAL_UINT(single.advances, blocks.advances);
        TEST_ASSERT_EQUAL_UINT(single.bin.samples, blocks.bin.samples);
    }
}

static void
assert_frequency_estimate(int16_t* samples, size_t n, uint16_t hz, float offset, unsigned seed) {
    rng_t random;
    rng_seed(&random, seed);
    const float actual = hz + offset;
    for (size_t i = 0; i < n; i++) {
        const float noise = seed == 1 ? 0
                                      : TEST_ESTIMATE_NOISE * TEST_NOISE_RMS_MATCH_SCALE
                                            * ((float)rng_next(&random) / (float)UINT32_MAX - 0.5F);
        samples[i] =
            (int16_t)lroundf(TEST_ESTIMATE_AMPLITUDE
                             * (sinf(MATH_TAU * actual * (float)i / AUDIO_OUT_SAMPLE_RATE_HZ + (float)seed) + noise));
    }
    audio_loopback_frequency_t state;
    audio_loopback_frequency_init(&state, hz);
    for (size_t i = 0; i < n; i++) {
        audio_loopback_frequency_feed(&state, samples + i, 1);
    }
    TEST_ASSERT_FLOAT_WITHIN(seed == 1 ? TEST_ESTIMATE_TOLERANCE_HZ : TEST_ESTIMATE_NOISE_TOLERANCE_HZ, actual,
                             audio_loopback_frequency_hz(&state));
}

static void
test_loopback_frequency_estimate(void) {
    static const float offsets[] = {0.0F, 3.7F, -11.0F};
    static const uint16_t bins[] = {250, 300, 500, TEST_HZ, 2000, 3000, 4000, 8000};
    const size_t n = audio_out_samples(TEST_ESTIMATE_MS);
    int16_t* samples = fixture(n);
    for (size_t b = 0; b < sizeof bins / sizeof bins[0]; b++) {
        for (size_t o = 0; o < sizeof offsets / sizeof offsets[0]; o++) {
            for (unsigned seed = 1; seed <= 4; seed++) {
                assert_frequency_estimate(samples, n, bins[b], offsets[o], seed);
            }
        }
    }
}

void
run_audio_tone_suite(void) {
    RUN_TEST(test_loopback_frequency_boundaries_and_blocks);
    RUN_TEST(test_loopback_frequency_estimate);
    RUN_TEST(test_loopback_measured_budget);
    RUN_TEST(test_tone_frequency);
    RUN_TEST(test_tone_chunks_and_volume);
}
SUITE_REGISTER(run_audio_tone_suite)

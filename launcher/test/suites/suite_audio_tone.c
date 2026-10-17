/* Portable synthesis checks against time, amplitude and chunking contracts. */
#include <math.h>
#include <stdlib.h>
#include "audio/audio_loopback.h"
#include "audio/audio_tone.h"
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

void
run_audio_tone_suite(void) {
    RUN_TEST(test_loopback_measured_budget);
    RUN_TEST(test_tone_frequency);
    RUN_TEST(test_tone_chunks_and_volume);
}
SUITE_REGISTER(run_audio_tone_suite)

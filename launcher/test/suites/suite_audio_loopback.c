#include <math.h>
#include "audio/audio_codec.h"
#include "audio/audio_loopback_detector.h"
#include "math/scalar/math_const.h"
#include "suites.h"
#include "unity.h"

#define TEST_LOOPBACK_NYQUIST_HARMONIC   3u
#define TEST_LOOPBACK_HZ                 1000u
#define TEST_LOOPBACK_MS                 500u
#define TEST_LOOPBACK_AMPLITUDE          (INT16_MAX / 2)
#define TEST_LOOPBACK_HARMONIC_RATIO     0.1F
#define TEST_LOOPBACK_TOLERANCE_DB       0.2F
#define TEST_LOOPBACK_EXPECTED_THD_DB    (-20.0F)
#define TEST_LOOPBACK_EXPECTED_PEAK_DBFS (-6.02F)
#define TEST_LOOPBACK_FRACTIONAL_HZ      1000.3F

static audio_loopback_detector_t
fixture(float hz, unsigned harmonic) {
    audio_loopback_detector_t detector;
    audio_loopback_detector_init(&detector, (uint16_t)hz, true);
    int16_t samples[AUDIO_CODEC_BLOCK_SAMPLES];
    const size_t total = audio_out_samples(TEST_LOOPBACK_MS);
    for (size_t offset = 0; offset < total; offset += AUDIO_CODEC_BLOCK_SAMPLES) {
        const size_t count = total - offset < AUDIO_CODEC_BLOCK_SAMPLES ? total - offset : AUDIO_CODEC_BLOCK_SAMPLES;
        for (size_t i = 0; i < count; i++) {
            const float phase = MATH_TAU * hz * (float)(offset + i) / AUDIO_OUT_SAMPLE_RATE_HZ;
            samples[i] = (int16_t)(TEST_LOOPBACK_AMPLITUDE
                                   * (sinf(phase) + TEST_LOOPBACK_HARMONIC_RATIO * sinf(harmonic * phase)));
        }
        audio_loopback_detector_feed(&detector, samples, count, true);
    }
    return detector;
}

static void
test_loopback_harmonics(void) {
    for (unsigned harmonic = AUDIO_LOOPBACK_FIRST_HARMONIC; harmonic <= AUDIO_LOOPBACK_HARMONICS; harmonic++) {
        audio_loopback_detector_t detector = fixture(TEST_LOOPBACK_HZ, harmonic);
        audio_loopback_result_t result;
        audio_loopback_detector_result(&detector, &result);
        TEST_ASSERT_FLOAT_WITHIN(TEST_LOOPBACK_TOLERANCE_DB, TEST_LOOPBACK_EXPECTED_THD_DB, result.thd_db);
        TEST_ASSERT_EQUAL_FLOAT(math_db_amplitude((float)detector.peak / INT16_MAX), result.peak_dbfs);
    }
}

static void
test_loopback_floor_and_cap(void) {
    const float frequencies[] = {TEST_LOOPBACK_HZ, TEST_LOOPBACK_HZ + 1, TEST_LOOPBACK_FRACTIONAL_HZ};
    for (size_t i = 0; i < sizeof frequencies / sizeof frequencies[0]; i++) {
        audio_loopback_detector_t detector = fixture(frequencies[i], 0);
        audio_loopback_result_t result;
        audio_loopback_detector_result(&detector, &result);
        TEST_ASSERT_LESS_THAN_FLOAT(AUDIO_LOOPBACK_THD_FLOOR_DB, result.thd_db);
        TEST_ASSERT_FLOAT_WITHIN(TEST_LOOPBACK_TOLERANCE_DB, TEST_LOOPBACK_EXPECTED_PEAK_DBFS, result.peak_dbfs);
    }
    audio_loopback_detector_t detector = fixture(TEST_LOOPBACK_HZ, AUDIO_LOOPBACK_HARMONICS + 1);
    audio_loopback_result_t result;
    audio_loopback_detector_result(&detector, &result);
    TEST_ASSERT_LESS_THAN_FLOAT(AUDIO_LOOPBACK_THD_FLOOR_DB, result.thd_db);
}

static void
test_loopback_edges(void) {
    audio_loopback_detector_t detector;
    const unsigned harmonic = TEST_LOOPBACK_NYQUIST_HARMONIC;
    audio_loopback_detector_init(&detector, AUDIO_OUT_NYQUIST_HZ / harmonic, true);
    TEST_ASSERT_EQUAL_UINT(harmonic - AUDIO_LOOPBACK_FIRST_HARMONIC, detector.harmonic_count);
    audio_loopback_detector_init(&detector, AUDIO_OUT_NYQUIST_HZ / harmonic - 1u, true);
    TEST_ASSERT_EQUAL_UINT(harmonic - AUDIO_LOOPBACK_FIRST_HARMONIC + 1u, detector.harmonic_count);
    audio_loopback_detector_init(&detector, (AUDIO_OUT_NYQUIST_HZ - 1u) / AUDIO_LOOPBACK_FIRST_HARMONIC, true);
    TEST_ASSERT_EQUAL_UINT(1, detector.harmonic_count);
    audio_loopback_detector_init(&detector, AUDIO_OUT_NYQUIST_HZ / AUDIO_LOOPBACK_FIRST_HARMONIC + 1u, true);
    audio_loopback_result_t result;
    audio_loopback_detector_result(&detector, &result);
    TEST_ASSERT_TRUE(isnan(result.thd_db));
    audio_loopback_detector_init(&detector, TEST_LOOPBACK_HZ, true);
    const int16_t silence[] = {0};
    audio_loopback_detector_feed(&detector, silence, 1, true);
    audio_loopback_detector_result(&detector, &result);
    TEST_ASSERT_TRUE(isnan(result.thd_db));
    TEST_ASSERT_EQUAL_FLOAT(MATH_DB_FLOOR, result.peak_dbfs);
}

static void
test_loopback_broadband_rms(void) {
    audio_loopback_detector_t detector;
    audio_loopback_detector_init(&detector, TEST_LOOPBACK_HZ, false);
    const int16_t samples[] = {TEST_LOOPBACK_AMPLITUDE, -TEST_LOOPBACK_AMPLITUDE};
    audio_loopback_detector_feed(&detector, samples, sizeof samples / sizeof samples[0], false);
    audio_loopback_result_t result;
    audio_loopback_detector_result(&detector, &result);
    TEST_ASSERT_FLOAT_WITHIN(TEST_LOOPBACK_TOLERANCE_DB, TEST_LOOPBACK_EXPECTED_PEAK_DBFS, result.noise_dbfs);
}

void
run_audio_loopback_suite(void) {
    RUN_TEST(test_loopback_broadband_rms);
    RUN_TEST(test_loopback_harmonics);
    RUN_TEST(test_loopback_floor_and_cap);
    RUN_TEST(test_loopback_edges);
}
SUITE_REGISTER(run_audio_loopback_suite)

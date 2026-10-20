/* A private capture reader exercises the production measurement without I2S. */
#ifndef DEVICE_BUILD
#define audio_loopback_measure test_loopback_measure
#define audio_in_read          test_loopback_read
#include "audio/audio_loopback.c"
#undef audio_loopback_measure
#undef audio_in_read

#include "math/scalar/math_const.h"
#include "suites.h"
#include "unity.h"

#define TEST_LOOPBACK_HZ                 1000u
#define TEST_LOOPBACK_MS                 200u
#define TEST_LOOPBACK_AMPLITUDE          (INT16_MAX / 2)
#define TEST_LOOPBACK_HARMONIC_RATIO     0.1F
#define TEST_LOOPBACK_TOLERANCE_DB       0.2F
#define TEST_LOOPBACK_EXPECTED_THD_DB    (-20.0F)
#define TEST_LOOPBACK_EXPECTED_PEAK_DBFS (-6.02F)

typedef struct {
    size_t position;
    uint16_t hz;
    bool distorted;
    bool silent;
    bool failed;
    int peak;
} loopback_fixture_t;

static loopback_fixture_t capture;

static void
fixture(uint16_t hz) {
    capture = (loopback_fixture_t){.hz = hz};
}

bool
test_loopback_read(int16_t* samples, size_t count) {
    if (capture.failed) {
        return false;
    }
    for (size_t i = 0; i < count; i++, capture.position++) {
        const double phase = MATH_TAU * capture.hz * capture.position / AUDIO_OUT_SAMPLE_RATE_HZ;
        double value = capture.silent ? 0 : TEST_LOOPBACK_AMPLITUDE * sin(phase);
        if (capture.distorted) {
            value +=
                TEST_LOOPBACK_AMPLITUDE * TEST_LOOPBACK_HARMONIC_RATIO * sin(AUDIO_LOOPBACK_FIRST_HARMONIC * phase);
        }
        samples[i] = (int16_t)value;
        if (capture.position < audio_out_samples(AUDIO_LOOPBACK_SETTLE_MS)) {
            samples[i] = INT16_MIN;
        } else {
            const int magnitude = samples[i] < 0 ? -(int)samples[i] : samples[i];
            if (magnitude > capture.peak) {
                capture.peak = magnitude;
            }
        }
    }
    return true;
}

static void
test_loopback_thd_and_peak(void) {
    fixture(TEST_LOOPBACK_HZ);
    capture.distorted = true;
    float db;
    audio_loopback_result_t result;
    TEST_ASSERT_TRUE(test_loopback_measure(TEST_LOOPBACK_HZ, TEST_LOOPBACK_MS, &db, &result));
    TEST_ASSERT_FLOAT_WITHIN(TEST_LOOPBACK_TOLERANCE_DB, TEST_LOOPBACK_EXPECTED_THD_DB, result.thd_db);
    const float expected_peak = AUDIO_LOOPBACK_AMPLITUDE_DB_SCALE * log10f((float)capture.peak / INT16_MAX);
    TEST_ASSERT_FLOAT_WITHIN(TEST_LOOPBACK_TOLERANCE_DB, expected_peak, result.peak_dbfs);
    TEST_ASSERT_FLOAT_WITHIN(TEST_LOOPBACK_TOLERANCE_DB, TEST_LOOPBACK_EXPECTED_PEAK_DBFS, db);
    fixture(TEST_LOOPBACK_HZ);
    TEST_ASSERT_TRUE(test_loopback_measure(TEST_LOOPBACK_HZ, TEST_LOOPBACK_MS, &db, &result));
    TEST_ASSERT_LESS_THAN_FLOAT(TEST_LOOPBACK_EXPECTED_THD_DB, result.thd_db);
}

static void
test_loopback_edges(void) {
    const uint16_t hz = AUDIO_OUT_NYQUIST_HZ / AUDIO_LOOPBACK_FIRST_HARMONIC + 1u;
    fixture(hz);
    float db;
    audio_loopback_result_t result;
    TEST_ASSERT_TRUE(test_loopback_measure(hz, TEST_LOOPBACK_MS, &db, &result));
    TEST_ASSERT_TRUE(isnan(result.thd_db));
    fixture(TEST_LOOPBACK_HZ);
    capture.silent = true;
    TEST_ASSERT_TRUE(test_loopback_measure(TEST_LOOPBACK_HZ, TEST_LOOPBACK_MS, &db, &result));
    TEST_ASSERT_TRUE(isnan(result.thd_db));
    TEST_ASSERT_EQUAL_FLOAT(GOERTZEL_LEVEL_FLOOR_DB, result.peak_dbfs);
    fixture(TEST_LOOPBACK_HZ);
    TEST_ASSERT_TRUE(test_loopback_measure(TEST_LOOPBACK_HZ, TEST_LOOPBACK_MS, &db, NULL));
    capture.failed = true;
    TEST_ASSERT_FALSE(test_loopback_measure(TEST_LOOPBACK_HZ, TEST_LOOPBACK_MS, &db, &result));
}

void
run_audio_loopback_suite(void) {
    RUN_TEST(test_loopback_thd_and_peak);
    RUN_TEST(test_loopback_edges);
}
SUITE_REGISTER(run_audio_loopback_suite)
#endif

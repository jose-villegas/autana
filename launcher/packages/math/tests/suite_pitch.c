#include <math.h>
#include "math/signal/pitch.h"
#include "testkit/suite.h"
#include "unity.h"

#define TEST_REFERENCE_HZ    256.0F
#define TEST_RELATIVE_ERROR  1e-6F
#define TEST_FIFTH_SEMITONES 7
#define TEST_RANGE_OCTAVES   4

static float
fixture(void) {
    return TEST_REFERENCE_HZ;
}

static void
test_pitch_intervals_and_reference(void) {
    const float reference = fixture();
    TEST_ASSERT_EQUAL_FLOAT(reference, pitch_semitone_hz(reference, 0));
    TEST_ASSERT_EQUAL_FLOAT(reference * 2, pitch_semitone_hz(reference, PITCH_SEMITONES_PER_OCTAVE));
    TEST_ASSERT_EQUAL_FLOAT(reference / 2, pitch_semitone_hz(reference, -PITCH_SEMITONES_PER_OCTAVE));
    const float fifth = powf(2, (float)TEST_FIFTH_SEMITONES / PITCH_SEMITONES_PER_OCTAVE);
    static const float scales[] = {0.25F, 1.0F, 3.0F};
    for (size_t i = 0; i < sizeof scales / sizeof scales[0]; i++) {
        const float expected = reference * scales[i] * fifth;
        TEST_ASSERT_FLOAT_WITHIN(expected * TEST_RELATIVE_ERROR, expected,
                                 pitch_semitone_hz(reference * scales[i], TEST_FIFTH_SEMITONES));
    }
}

static void
test_pitch_roundtrip(void) {
    const float reference = fixture();
    const int range = TEST_RANGE_OCTAVES * PITCH_SEMITONES_PER_OCTAVE;
    for (int offset = -range; offset <= range; offset++) {
        const float hz = reference * powf(2, (float)offset / PITCH_SEMITONES_PER_OCTAVE);
        const float semitones = pitch_hz_semitones(reference, hz);
        TEST_ASSERT_FLOAT_WITHIN(hz * TEST_RELATIVE_ERROR, hz, pitch_semitone_hz(reference, semitones));
    }
}

static void
test_pitch_invalid_inverse(void) {
    const float reference = fixture();
    static const float invalid[] = {0, -1};
    for (size_t i = 0; i < sizeof invalid / sizeof invalid[0]; i++) {
        TEST_ASSERT_TRUE(isnan(pitch_hz_semitones(reference, invalid[i])));
        TEST_ASSERT_TRUE(isnan(pitch_hz_semitones(invalid[i], reference)));
    }
}

void
run_pitch_suite(void) {
    RUN_TEST(test_pitch_intervals_and_reference);
    RUN_TEST(test_pitch_roundtrip);
    RUN_TEST(test_pitch_invalid_inverse);
}
SUITE_REGISTER(run_pitch_suite)

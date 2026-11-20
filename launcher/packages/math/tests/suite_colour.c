#include <math.h>
#include <stdint.h>

#include "math/colour/oklab.h"
#include "math/colour/srgb.h"
#include "testkit/suite.h"
#include "unity.h"

#define BYTE_MAX             255
#define CURVE_TOLERANCE      1.0E-6F
#define OKLAB_TOLERANCE      1.0E-4F
#define ROUND_TRIP_TOLERANCE 1.0E-5F
#define MID_GREY_LINEAR      0.18F
#define MID_GREY_ENCODED     0.46135613F /* IEC 61966-2-1 curve at 18 % */
#define HALF_ENCODED_LINEAR  0.21404114F
#define BYTE_128_LINEAR      0.21586050F
#define HALF_LINEAR_BYTE     188

static double
reference_decode(double encoded) {
    return encoded <= 0.04045 ? encoded / 12.92 : pow((encoded + 0.055) / 1.055, 2.4);
}

static void
test_curve_ends_and_reference_points(void) {
    TEST_ASSERT_EQUAL_FLOAT(0.0F, srgbf_decode(0.0F));
    TEST_ASSERT_FLOAT_WITHIN(CURVE_TOLERANCE, 1.0F, srgbf_decode(1.0F));
    TEST_ASSERT_FLOAT_WITHIN(CURVE_TOLERANCE, 1.0F, srgbf_encode(1.0F));
    TEST_ASSERT_FLOAT_WITHIN(CURVE_TOLERANCE, HALF_ENCODED_LINEAR, srgbf_decode(0.5F));
    TEST_ASSERT_FLOAT_WITHIN(CURVE_TOLERANCE, MID_GREY_ENCODED, srgbf_encode(MID_GREY_LINEAR));
}

static void
test_linear_segment_meets_the_power_segment(void) {
    TEST_ASSERT_FLOAT_WITHIN(CURVE_TOLERANCE, SRGB_LINEAR_CUTOFF, srgbf_decode(SRGB_ENCODED_CUTOFF));
    TEST_ASSERT_FLOAT_WITHIN(CURVE_TOLERANCE, SRGB_ENCODED_CUTOFF, srgbf_encode(SRGB_LINEAR_CUTOFF));
}

static void
test_encode_byte_rounds_and_clamps(void) {
    TEST_ASSERT_EQUAL_UINT8(0, srgbf_encode_byte(-1.0F));
    TEST_ASSERT_EQUAL_UINT8(0, srgbf_encode_byte(NAN));
    TEST_ASSERT_EQUAL_UINT8(BYTE_MAX, srgbf_encode_byte(2.0F));
    TEST_ASSERT_EQUAL_UINT8(HALF_LINEAR_BYTE, srgbf_encode_byte(0.5F));
}

static void
test_decode_table_is_the_curve(void) {
    TEST_ASSERT_FLOAT_WITHIN(CURVE_TOLERANCE, BYTE_128_LINEAR, srgbf_decode_byte(128));
    for (int i = 0; i <= BYTE_MAX; i++) {
        const float expected = (float)reference_decode((double)i / BYTE_MAX);
        TEST_ASSERT_EQUAL_FLOAT(expected, srgbf_decode_byte((uint8_t)i));
    }
}

static void
test_every_byte_round_trips(void) {
    for (int i = 0; i <= BYTE_MAX; i++) {
        TEST_ASSERT_EQUAL_UINT8(i, srgbf_encode_byte(srgbf_decode_byte((uint8_t)i)));
    }
}

/* Ottosson's sRGB primaries, as CSS Color 4 tabulates them. */
static void
test_primaries_match_published_oklab(void) {
    const vec3f_t primaries[] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {1, 1, 1}};
    const oklabf_t expected[] = {{0.627955F, 0.224863F, 0.125846F},
                                 {0.866440F, -0.233888F, 0.179498F},
                                 {0.452014F, -0.032457F, -0.311528F},
                                 {1.0F, 0.0F, 0.0F}};
    for (int i = 0; i < 4; i++) {
        const oklabf_t got = oklabf_from_linear(primaries[i]);
        TEST_ASSERT_FLOAT_WITHIN(OKLAB_TOLERANCE, expected[i].l, got.l);
        TEST_ASSERT_FLOAT_WITHIN(OKLAB_TOLERANCE, expected[i].a, got.a);
        TEST_ASSERT_FLOAT_WITHIN(OKLAB_TOLERANCE, expected[i].b, got.b);
    }
}

static void
test_oklab_round_trips(void) {
    const vec3f_t colours[] = {{0.2F, 0.5F, 0.8F}, {0.9F, 0.1F, 0.05F}, {0.0F, 0.0F, 0.0F}, {0.33F, 0.33F, 0.33F}};
    for (int i = 0; i < 4; i++) {
        const vec3f_t back = oklabf_to_linear(oklabf_from_linear(colours[i]));
        TEST_ASSERT_FLOAT_WITHIN(ROUND_TRIP_TOLERANCE, colours[i].x, back.x);
        TEST_ASSERT_FLOAT_WITHIN(ROUND_TRIP_TOLERANCE, colours[i].y, back.y);
        TEST_ASSERT_FLOAT_WITHIN(ROUND_TRIP_TOLERANCE, colours[i].z, back.z);
    }
}

void
suite_colour(void) {
    RUN_TEST(test_curve_ends_and_reference_points);
    RUN_TEST(test_linear_segment_meets_the_power_segment);
    RUN_TEST(test_encode_byte_rounds_and_clamps);
    RUN_TEST(test_decode_table_is_the_curve);
    RUN_TEST(test_every_byte_round_trips);
    RUN_TEST(test_primaries_match_published_oklab);
    RUN_TEST(test_oklab_round_trips);
}
SUITE_REGISTER(suite_colour)

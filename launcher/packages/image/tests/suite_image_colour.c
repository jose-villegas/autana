#include <math.h>
#include <stdint.h>

#include "image/colour/oklab.h"
#include "image/colour/srgb.h"
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

#define SEGMENT_STEP         1.0E-4F
#define OUT_OF_GAMUT_CHROMA  0.4F

/* Double references stay on the host: the device build keeps soft double out. */
#ifndef ESP_PLATFORM
#define LINEAR_SEGMENT_INPUT 0.001F

static double
reference_encode(double linear) {
    return linear <= 0.0031308 ? linear * 12.92 : (1.055 * pow(linear, 1.0 / 2.4)) - 0.055;
}

static double
reference_decode(double encoded) {
    return encoded <= 0.04045 ? encoded / 12.92 : pow((encoded + 0.055) / 1.055, 2.4);
}
#endif

static void
test_curve_ends_and_reference_points(void) {
    TEST_ASSERT_EQUAL_FLOAT(0.0F, srgbf_decode(0.0F));
    TEST_ASSERT_FLOAT_WITHIN(CURVE_TOLERANCE, 1.0F, srgbf_decode(1.0F));
    TEST_ASSERT_FLOAT_WITHIN(CURVE_TOLERANCE, 1.0F, srgbf_encode(1.0F));
    TEST_ASSERT_FLOAT_WITHIN(CURVE_TOLERANCE, HALF_ENCODED_LINEAR, srgbf_decode(0.5F));
    TEST_ASSERT_FLOAT_WITHIN(CURVE_TOLERANCE, MID_GREY_ENCODED, srgbf_encode(MID_GREY_LINEAR));
    TEST_ASSERT_FLOAT_WITHIN(CURVE_TOLERANCE, BYTE_128_LINEAR, srgbf_decode_byte(128));
}

static void
test_linear_segment_meets_the_power_segment(void) {
    TEST_ASSERT_FLOAT_WITHIN(CURVE_TOLERANCE, SRGB_LINEAR_CUTOFF, srgbf_decode(SRGB_ENCODED_CUTOFF));
    TEST_ASSERT_FLOAT_WITHIN(CURVE_TOLERANCE, SRGB_ENCODED_CUTOFF, srgbf_encode(SRGB_LINEAR_CUTOFF));
}

static void
test_encode_clamps_before_the_curve(void) {
    TEST_ASSERT_EQUAL_FLOAT(0.0F, srgbf_encode(NAN));
    TEST_ASSERT_EQUAL_FLOAT(0.0F, srgbf_encode(-0.0F));
    TEST_ASSERT_EQUAL_FLOAT(0.0F, srgbf_encode(-1.0F));
    TEST_ASSERT_EQUAL_FLOAT(1.0F, srgbf_encode(1.0F + SEGMENT_STEP));
    TEST_ASSERT_EQUAL_FLOAT(1.0F, srgbf_encode(INFINITY));
}

#ifndef ESP_PLATFORM
static void
test_each_side_of_the_cutoff_takes_its_own_segment(void) {
    const float points[] = {LINEAR_SEGMENT_INPUT, SRGB_LINEAR_CUTOFF - SEGMENT_STEP, SRGB_LINEAR_CUTOFF + SEGMENT_STEP,
                            MID_GREY_LINEAR};
    for (int i = 0; i < 4; i++) {
        TEST_ASSERT_FLOAT_WITHIN(CURVE_TOLERANCE, (float)reference_encode(points[i]), srgbf_encode(points[i]));
        const float encoded = (float)reference_encode(points[i]);
        TEST_ASSERT_FLOAT_WITHIN(CURVE_TOLERANCE, points[i], srgbf_decode(encoded));
    }
}
#endif

static void
test_encode_byte_rounds_and_clamps(void) {
    TEST_ASSERT_EQUAL_UINT8(0, srgbf_encode_byte(-1.0F));
    TEST_ASSERT_EQUAL_UINT8(0, srgbf_encode_byte(NAN));
    TEST_ASSERT_EQUAL_UINT8(BYTE_MAX, srgbf_encode_byte(2.0F));
    TEST_ASSERT_EQUAL_UINT8(HALF_LINEAR_BYTE, srgbf_encode_byte(0.5F));
}

#ifndef ESP_PLATFORM
static void
test_every_byte_decodes_to_the_curve(void) {
    for (int i = 0; i <= BYTE_MAX; i++) {
        const float expected = (float)reference_decode((double)i / BYTE_MAX);
        TEST_ASSERT_EQUAL_FLOAT(expected, srgbf_decode_byte((uint8_t)i));
    }
}
#endif

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
test_published_oklab_returns_its_primary(void) {
    const oklabf_t red = {0.627955F, 0.224863F, 0.125846F};
    const vec3f_t got = oklabf_to_linear(red);
    TEST_ASSERT_FLOAT_WITHIN(OKLAB_TOLERANCE, 1.0F, got.x);
    TEST_ASSERT_FLOAT_WITHIN(OKLAB_TOLERANCE, 0.0F, got.y);
    TEST_ASSERT_FLOAT_WITHIN(OKLAB_TOLERANCE, 0.0F, got.z);
}

static void
test_greys_have_no_chroma_and_out_of_gamut_is_not_clamped(void) {
    const oklabf_t grey = oklabf_from_linear((vec3f_t){MID_GREY_LINEAR, MID_GREY_LINEAR, MID_GREY_LINEAR});
    TEST_ASSERT_FLOAT_WITHIN(OKLAB_TOLERANCE, 0.0F, grey.a);
    TEST_ASSERT_FLOAT_WITHIN(OKLAB_TOLERANCE, 0.0F, grey.b);
    const vec3f_t vivid = oklabf_to_linear((oklabf_t){grey.l, OUT_OF_GAMUT_CHROMA, 0.0F});
    TEST_ASSERT_TRUE(vivid.x > 1.0F || vivid.y < 0.0F || vivid.z < 0.0F);
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
suite_image_colour(void) {
    RUN_TEST(test_curve_ends_and_reference_points);
    RUN_TEST(test_linear_segment_meets_the_power_segment);
    RUN_TEST(test_encode_clamps_before_the_curve);
#ifndef ESP_PLATFORM
    RUN_TEST(test_each_side_of_the_cutoff_takes_its_own_segment);
#endif
    RUN_TEST(test_encode_byte_rounds_and_clamps);
#ifndef ESP_PLATFORM
    RUN_TEST(test_every_byte_decodes_to_the_curve);
#endif
    RUN_TEST(test_every_byte_round_trips);
    RUN_TEST(test_primaries_match_published_oklab);
    RUN_TEST(test_published_oklab_returns_its_primary);
    RUN_TEST(test_greys_have_no_chroma_and_out_of_gamut_is_not_clamped);
    RUN_TEST(test_oklab_round_trips);
}
SUITE_REGISTER(suite_image_colour)

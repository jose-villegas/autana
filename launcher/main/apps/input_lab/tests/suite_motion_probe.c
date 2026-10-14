#include "suites.h"
#include "unity.h"

#include "apps/input_lab/motion_probe.h"

#define ANGLE_TOLERANCE_DEG 0.02F
#define COUNT_TOLERANCE     0.02F
#define TEST_RING_RADIUS    80

static void
test_flat_upright_face_down_and_diagonals(void) {
    const int g = INPUT_ACCEL_ONE_G;
    const vec3i_t samples[] = {{0, 0, -g}, {0, g, 0}, {0, 0, g}, {g, 0, -g}, {0, -g, -g}, {g, 0, g}};
    const float expected[] = {0,
                              (MOTION_PROBE_FULL_TURN_DEG / 4),
                              2 * (MOTION_PROBE_FULL_TURN_DEG / 4),
                              (MOTION_PROBE_FULL_TURN_DEG / 4) / 2,
                              (MOTION_PROBE_FULL_TURN_DEG / 4) / 2,
                              3 * (MOTION_PROBE_FULL_TURN_DEG / 4) / 2};
    for (unsigned i = 0; i < sizeof samples / sizeof samples[0]; i++) {
        TEST_ASSERT_FLOAT_WITHIN(ANGLE_TOLERANCE_DEG, expected[i], motion_probe_measure(samples[i]).tilt_deg);
    }
}

static void
test_bearing_axes_and_magnitudes(void) {
    const int g = INPUT_ACCEL_ONE_G;
    const vec3i_t samples[] = {{g, 0, 0}, {0, g, 0}, {-g, 0, 0}, {0, -g, 0}};
    for (unsigned i = 0; i < sizeof samples / sizeof samples[0]; i++) {
        const motion_probe_t probe = motion_probe_measure(samples[i]);
        TEST_ASSERT_FLOAT_WITHIN(ANGLE_TOLERANCE_DEG, i * (MOTION_PROBE_FULL_TURN_DEG / 4), probe.bearing_deg);
        TEST_ASSERT_FLOAT_WITHIN(COUNT_TOLERANCE, g, probe.plane_counts);
        TEST_ASSERT_FLOAT_WITHIN(COUNT_TOLERANCE, g, probe.magnitude_counts);
    }
}

static void
test_near_flat_preserves_small_plane_signal(void) {
    const motion_probe_t probe = motion_probe_measure((vec3i_t){1, 1, -INPUT_ACCEL_ONE_G});
    TEST_ASSERT_FLOAT_WITHIN(COUNT_TOLERANCE, 1.414213562F, probe.plane_counts);
    TEST_ASSERT_FLOAT_WITHIN(ANGLE_TOLERANCE_DEG, (MOTION_PROBE_FULL_TURN_DEG / 4) / 2, probe.bearing_deg);
    TEST_ASSERT_TRUE(probe.tilt_deg > 0 && probe.tilt_deg < 1);
    TEST_ASSERT_FLOAT_WITHIN(COUNT_TOLERANCE, INPUT_ACCEL_ONE_G, probe.magnitude_counts);
    TEST_ASSERT_EQUAL_FLOAT(0, motion_probe_measure((vec3i_t){0, 0, -INPUT_ACCEL_ONE_G}).bearing_deg);
}

static void
test_bubble_uses_inverse_shell_rotation(void) {
    const vec2i_t expected[] = {
        {TEST_RING_RADIUS, 0}, {0, -TEST_RING_RADIUS}, {-TEST_RING_RADIUS, 0}, {0, TEST_RING_RADIUS}};
    for (unsigned q = 0; q < sizeof expected / sizeof expected[0]; q++) {
        const vec2i_t dot = motion_probe_bubble(INPUT_ACCEL_ONE_G, 0, TEST_RING_RADIUS, (int)q);
        TEST_ASSERT_EQUAL_INT(expected[q].x, dot.x);
        TEST_ASSERT_EQUAL_INT(expected[q].y, dot.y);
        const vec2i_t down = motion_probe_bubble(0, INPUT_ACCEL_ONE_G, TEST_RING_RADIUS, (int)q);
        TEST_ASSERT_EQUAL_INT(-expected[q].y, down.x);
        TEST_ASSERT_EQUAL_INT(expected[q].x, down.y);
    }
    const vec2i_t half = motion_probe_bubble(INPUT_ACCEL_ONE_G / 2, -INPUT_ACCEL_ONE_G / 2, TEST_RING_RADIUS, 0);
    TEST_ASSERT_EQUAL_INT(TEST_RING_RADIUS / 2, half.x);
    TEST_ASSERT_EQUAL_INT(-TEST_RING_RADIUS / 2, half.y);
}

static void
test_ring_extents(void) {
    TEST_ASSERT_EQUAL_INT(TEST_RING_RADIUS, motion_probe_ring_extent(TEST_RING_RADIUS, 0));
    TEST_ASSERT_EQUAL_INT(0, motion_probe_ring_extent(TEST_RING_RADIUS, TEST_RING_RADIUS));
    TEST_ASSERT_EQUAL_INT(motion_probe_ring_extent(TEST_RING_RADIUS, TEST_RING_RADIUS / 2),
                          motion_probe_ring_extent(TEST_RING_RADIUS, -TEST_RING_RADIUS / 2));
}

static void
test_bubble_rounding_and_unclamped_signal(void) {
    const vec2i_t expected[] = {{-27, -27}, {-27, 27}, {27, 27}};
    for (int q = 1; q < 4; q++) {
        const vec2i_t dot = motion_probe_bubble(INPUT_ACCEL_ONE_G / 3, -INPUT_ACCEL_ONE_G / 3, TEST_RING_RADIUS, q);
        TEST_ASSERT_EQUAL_INT(expected[q - 1].x, dot.x);
        TEST_ASSERT_EQUAL_INT(expected[q - 1].y, dot.y);
    }
    const vec2i_t dot = motion_probe_bubble(2 * INPUT_ACCEL_ONE_G, -2 * INPUT_ACCEL_ONE_G, TEST_RING_RADIUS, 0);
    TEST_ASSERT_EQUAL_INT(2 * TEST_RING_RADIUS, dot.x);
    TEST_ASSERT_EQUAL_INT(-2 * TEST_RING_RADIUS, dot.y);
}

static void
test_ring_extent_rounding(void) {
    TEST_ASSERT_EQUAL_INT(69, motion_probe_ring_extent(TEST_RING_RADIUS, 41));
    TEST_ASSERT_EQUAL_INT(68, motion_probe_ring_extent(TEST_RING_RADIUS, 42));
}

static void
test_tilt_scale_invariance(void) {
    for (int scale = 1; scale <= 3; scale++) {
        const motion_probe_t probe = motion_probe_measure((vec3i_t){100 * scale, 0, -1000 * scale});
        TEST_ASSERT_FLOAT_WITHIN(ANGLE_TOLERANCE_DEG, 5.710593F, probe.tilt_deg);
    }
}

static void
test_bearing_rounds_and_wraps(void) {
    TEST_ASSERT_EQUAL_INT(0, motion_probe_measure((vec3i_t){10000, -70, 0}).bearing_deg);
}

static void
test_fixed_point_formatter(void) {
    const int values[] = {-1, 0, 999, -1000, 1000, -1, 0, 19, -19};
    const int digits[] = {3, 3, 3, 3, 3, 1, 1, 1, 1};
    const char* expected[] = {"-0.001", "+0.000", "+0.999", "-1.000", "+1.000", "-0.1", "+0.0", "+1.9", "-1.9"};
    char text[16];
    for (size_t i = 0; i < sizeof values / sizeof values[0]; i++) {
        motion_probe_format_fixed(text, sizeof text, values[i], digits[i], true);
        TEST_ASSERT_EQUAL_STRING(expected[i], text);
    }
    motion_probe_format_fixed(text, sizeof text, 1000, 3, false);
    TEST_ASSERT_EQUAL_STRING("1.000", text);
}

void
run_motion_probe_suite(void) {
    RUN_TEST(test_bubble_rounding_and_unclamped_signal);
    RUN_TEST(test_ring_extent_rounding);
    RUN_TEST(test_tilt_scale_invariance);
    RUN_TEST(test_bearing_rounds_and_wraps);
    RUN_TEST(test_fixed_point_formatter);
    RUN_TEST(test_flat_upright_face_down_and_diagonals);
    RUN_TEST(test_bearing_axes_and_magnitudes);
    RUN_TEST(test_near_flat_preserves_small_plane_signal);
    RUN_TEST(test_bubble_uses_inverse_shell_rotation);
    RUN_TEST(test_ring_extents);
}

SUITE_REGISTER(run_motion_probe_suite);

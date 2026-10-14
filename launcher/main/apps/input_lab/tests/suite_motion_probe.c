#include "suites.h"
#include "unity.h"

#include "apps/input_lab/motion_probe.h"

#define ANGLE_TOLERANCE_DEG 0.02F
#define COUNT_TOLERANCE     0.02F
#define TEST_RING_RADIUS    80

static void
fixture(void) {}

static void
test_flat_upright_face_down_and_diagonals(void) {
    fixture();
    const int g = INPUT_ACCEL_ONE_G;
    const vec3i_t samples[] = {{0, 0, -g}, {0, g, 0}, {0, 0, g}, {g, 0, -g}, {0, -g, -g}, {g, 0, g}};
    const float expected[] = {0,
                              MOTION_PROBE_UPRIGHT_DEG,
                              2 * MOTION_PROBE_UPRIGHT_DEG,
                              MOTION_PROBE_UPRIGHT_DEG / 2,
                              MOTION_PROBE_UPRIGHT_DEG / 2,
                              3 * MOTION_PROBE_UPRIGHT_DEG / 2};
    for (unsigned i = 0; i < sizeof samples / sizeof samples[0]; i++) {
        TEST_ASSERT_FLOAT_WITHIN(ANGLE_TOLERANCE_DEG, expected[i], motion_probe_measure(samples[i]).tilt_deg);
    }
}

static void
test_bearing_axes_and_magnitudes(void) {
    fixture();
    const int g = INPUT_ACCEL_ONE_G;
    const vec3i_t samples[] = {{g, 0, 0}, {0, g, 0}, {-g, 0, 0}, {0, -g, 0}};
    for (unsigned i = 0; i < sizeof samples / sizeof samples[0]; i++) {
        const motion_probe_t probe = motion_probe_measure(samples[i]);
        TEST_ASSERT_FLOAT_WITHIN(ANGLE_TOLERANCE_DEG, i * MOTION_PROBE_UPRIGHT_DEG, probe.bearing_deg);
        TEST_ASSERT_FLOAT_WITHIN(COUNT_TOLERANCE, g, probe.plane_counts);
        TEST_ASSERT_FLOAT_WITHIN(COUNT_TOLERANCE, g, probe.magnitude_counts);
    }
}

static void
test_near_flat_preserves_small_plane_signal(void) {
    fixture();
    const motion_probe_t probe = motion_probe_measure((vec3i_t){1, 1, -INPUT_ACCEL_ONE_G});
    TEST_ASSERT_FLOAT_WITHIN(COUNT_TOLERANCE, 1.414213562F, probe.plane_counts);
    TEST_ASSERT_FLOAT_WITHIN(ANGLE_TOLERANCE_DEG, MOTION_PROBE_UPRIGHT_DEG / 2, probe.bearing_deg);
    TEST_ASSERT_TRUE(probe.tilt_deg > 0 && probe.tilt_deg < 1);
    TEST_ASSERT_FLOAT_WITHIN(COUNT_TOLERANCE, INPUT_ACCEL_ONE_G, probe.magnitude_counts);
    TEST_ASSERT_EQUAL_FLOAT(0, motion_probe_measure((vec3i_t){0, 0, -INPUT_ACCEL_ONE_G}).bearing_deg);
}

static void
test_bubble_uses_inverse_shell_rotation(void) {
    fixture();
    const vec2i_t expected[] = {
        {TEST_RING_RADIUS, 0}, {0, -TEST_RING_RADIUS}, {-TEST_RING_RADIUS, 0}, {0, TEST_RING_RADIUS}};
    for (unsigned q = 0; q < sizeof expected / sizeof expected[0]; q++) {
        const vec2i_t dot = motion_probe_bubble(INPUT_ACCEL_ONE_G, 0, TEST_RING_RADIUS, q);
        TEST_ASSERT_EQUAL_INT(expected[q].x, dot.x);
        TEST_ASSERT_EQUAL_INT(expected[q].y, dot.y);
        const vec2i_t down = motion_probe_bubble(0, INPUT_ACCEL_ONE_G, TEST_RING_RADIUS, q);
        TEST_ASSERT_EQUAL_INT(-expected[q].y, down.x);
        TEST_ASSERT_EQUAL_INT(expected[q].x, down.y);
    }
    const vec2i_t half = motion_probe_bubble(INPUT_ACCEL_ONE_G / 2, -INPUT_ACCEL_ONE_G / 2, TEST_RING_RADIUS, 0);
    TEST_ASSERT_EQUAL_INT(TEST_RING_RADIUS / 2, half.x);
    TEST_ASSERT_EQUAL_INT(-TEST_RING_RADIUS / 2, half.y);
}

static void
test_ring_extents(void) {
    fixture();
    TEST_ASSERT_EQUAL_INT(TEST_RING_RADIUS, motion_probe_ring_extent(TEST_RING_RADIUS, 0));
    TEST_ASSERT_EQUAL_INT(0, motion_probe_ring_extent(TEST_RING_RADIUS, TEST_RING_RADIUS));
    TEST_ASSERT_EQUAL_INT(motion_probe_ring_extent(TEST_RING_RADIUS, TEST_RING_RADIUS / 2),
                          motion_probe_ring_extent(TEST_RING_RADIUS, -TEST_RING_RADIUS / 2));
}

void
run_motion_probe_suite(void) {
    RUN_TEST(test_flat_upright_face_down_and_diagonals);
    RUN_TEST(test_bearing_axes_and_magnitudes);
    RUN_TEST(test_near_flat_preserves_small_plane_signal);
    RUN_TEST(test_bubble_uses_inverse_shell_rotation);
    RUN_TEST(test_ring_extents);
}

SUITE_REGISTER(run_motion_probe_suite);

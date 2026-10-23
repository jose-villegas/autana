/* shake reader: direction, timing and deliberate-motion thresholds. */
#include <math.h>

#include "suites.h"
#include "unity.h"

#include "input/motion/gravity.h"
#include "input/motion/shake.h"
#include "math/scalar/math_const.h"

#define TEST_TOLERANCE          0.0001F
#define TEST_RATE_REL_TOLERANCE 0.05F
#define TEST_STEP_TOLERANCE_MG  0.01F
#define TEST_SINE_HZ            4.0F
#define TEST_GENTLE_G           0.3F
#define TEST_HARD_G             2.0F
#define TEST_FRAME_US           22000U
#define TEST_DURATION_US        (4U * MATH_US_PER_S)
#define TEST_SETTLE_US          (2U * MATH_US_PER_S)

static shake_t
fixture(void) {
    shake_t s;
    shake_reset(&s);
    return s;
}

static float
signal(uint32_t us, float amplitude) {
    return amplitude * sinf(2.0F * MATH_PI * TEST_SINE_HZ * (float)us / MATH_US_PER_S);
}

static void
test_still_and_zero_dt(void) {
    shake_t s = fixture();
    shake_update(&s, (vec3f_t){0}, TEST_FRAME_US);
    TEST_ASSERT_EQUAL_FLOAT(0, shake_level(&s));
    s.smooth_mg = SHAKE_FULL_MG;
    shake_update(&s, (vec3f_t){0}, 0);
    TEST_ASSERT_EQUAL_FLOAT(SHAKE_FULL_MG, s.smooth_mg);
    shake_reset(&s);
    TEST_ASSERT_EQUAL_FLOAT(0, s.smooth_mg);
}

static void
test_level_range(void) {
    shake_t s = fixture();
    const float values[] = {0, SHAKE_START_MG, (SHAKE_START_MG + SHAKE_FULL_MG) / 2.0F, SHAKE_FULL_MG,
                            SHAKE_FULL_MG * 2.0F};
    const float expected[] = {0, 0, 0.5F, 1, 1};
    for (size_t i = 0; i < sizeof values / sizeof values[0]; i++) {
        s.smooth_mg = values[i];
        TEST_ASSERT_FLOAT_WITHIN(TEST_TOLERANCE, expected[i], shake_level(&s));
    }
}

static void
test_sine_axes(void) {
    shake_t along = fixture();
    shake_t across = fixture();
    for (uint32_t us = 0; us < TEST_DURATION_US; us += INPUT_MOTION_SAMPLE_US) {
        const float value = signal(us, TEST_GENTLE_G);
        shake_update(&along, (vec3f_t){0, 0, value}, INPUT_MOTION_SAMPLE_US);
        shake_update(&across, (vec3f_t){value, 0, 0}, INPUT_MOTION_SAMPLE_US);
        TEST_ASSERT_FLOAT_WITHIN(TEST_TOLERANCE, along.smooth_mg, across.smooth_mg);
        TEST_ASSERT_FLOAT_WITHIN(TEST_TOLERANCE, shake_level(&along), shake_level(&across));
    }
    TEST_ASSERT_GREATER_THAN(0, across.smooth_mg);
}

static void
test_frame_and_sample_steps(void) {
    shake_t sample = fixture();
    shake_t frame = fixture();
    for (uint32_t us = 0; us < TEST_DURATION_US; us += TEST_FRAME_US) {
        const vec3f_t linear = {signal(us, TEST_HARD_G), 0, 0};
        uint32_t remaining_us = TEST_FRAME_US;
        while (remaining_us > 0) {
            const uint32_t dt_us = remaining_us < INPUT_MOTION_SAMPLE_US ? remaining_us : INPUT_MOTION_SAMPLE_US;
            shake_update(&sample, linear, dt_us);
            remaining_us -= dt_us;
        }
        shake_update(&frame, linear, TEST_FRAME_US);
        TEST_ASSERT_FLOAT_WITHIN(TEST_STEP_TOLERANCE_MG, sample.smooth_mg, frame.smooth_mg);
        TEST_ASSERT_FLOAT_WITHIN(TEST_TOLERANCE, shake_level(&sample), shake_level(&frame));
    }
}

static void
mean_signal(uint32_t interval_us, float* smooth_mg, float* level) {
    shake_t s = fixture();
    float total_mg_us = 0;
    float total_level_us = 0;
    for (uint32_t us = 0; us < TEST_DURATION_US;) {
        const uint32_t dt_us = interval_us < TEST_DURATION_US - us ? interval_us : TEST_DURATION_US - us;
        us += dt_us;
        shake_update(&s, (vec3f_t){signal(us, TEST_HARD_G), 0, 0}, dt_us);
        if (us > TEST_SETTLE_US) {
            const uint32_t weight_us = dt_us < us - TEST_SETTLE_US ? dt_us : us - TEST_SETTLE_US;
            total_mg_us += s.smooth_mg * weight_us;
            total_level_us += shake_level(&s) * weight_us;
        }
    }
    *smooth_mg = total_mg_us / (TEST_DURATION_US - TEST_SETTLE_US);
    *level = total_level_us / (TEST_DURATION_US - TEST_SETTLE_US);
}

static void
test_frame_and_sample_rates(void) {
    float sample_mg, sample_level, frame_mg, frame_level;
    mean_signal(INPUT_MOTION_SAMPLE_US, &sample_mg, &sample_level);
    mean_signal(TEST_FRAME_US, &frame_mg, &frame_level);
    TEST_ASSERT_FLOAT_WITHIN(TEST_RATE_REL_TOLERANCE * sample_mg, sample_mg, frame_mg);
    TEST_ASSERT_FLOAT_WITHIN(TEST_RATE_REL_TOLERANCE * sample_level, sample_level, frame_level);
}

static void
test_dt_clamp_and_step(void) {
    shake_t s = fixture();
    shake_t clamped = fixture();
    const vec3f_t linear = {1, 0, 0};
    shake_update(&s, linear, GRAVITY_MAX_DT_US);
    const float expected = SHAKE_MG_PER_G * (1.0F - expf(-(float)GRAVITY_MAX_DT_US / (SHAKE_TAU_MS * MATH_US_PER_MS)));
    TEST_ASSERT_FLOAT_WITHIN(TEST_TOLERANCE, expected, s.smooth_mg);
    shake_update(&clamped, linear, GRAVITY_MAX_DT_US * 2U);
    TEST_ASSERT_EQUAL_FLOAT(s.smooth_mg, clamped.smooth_mg);
}

void
run_shake_suite(void) {
    RUN_TEST(test_still_and_zero_dt);
    RUN_TEST(test_level_range);
    RUN_TEST(test_sine_axes);
    RUN_TEST(test_frame_and_sample_steps);
    RUN_TEST(test_frame_and_sample_rates);
    RUN_TEST(test_dt_clamp_and_step);
}

SUITE_REGISTER(run_shake_suite);

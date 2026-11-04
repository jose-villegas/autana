#include <math.h>

#include "math/scalar/math_const.h"
#include "math/signal/smooth.h"
#include "testkit/suite.h"
#include "unity.h"

#define HALF_LIFE_S 0.35F
#define STEP_S      0.5F
#define TOLERANCE   1.0E-6F

static float
fixture(void) {
    return HALF_LIFE_S;
}

static void
test_one_half_life_leaves_half(void) {
    const float half_life = fixture();
    TEST_ASSERT_FLOAT_WITHIN(TOLERANCE, 0.5F, smooth_decay(half_life, half_life));
    TEST_ASSERT_EQUAL_FLOAT(1.0F, smooth_decay(0.0F, half_life));
}

static void
test_alpha_at_zero_and_one_time_constant(void) {
    const float tau = fixture();
    TEST_ASSERT_EQUAL_FLOAT(0.0F, smooth_alpha(0.0F, tau));
    TEST_ASSERT_FLOAT_WITHIN(TOLERANCE, 1.0F - expf(-1.0F), smooth_alpha(tau, tau));
}

static void
test_time_constant_matches_half_life(void) {
    const float half_life = fixture();
    const float tau = half_life / MATH_LN2;
    TEST_ASSERT_FLOAT_WITHIN(TOLERANCE, 1.0F - smooth_decay(STEP_S, half_life), smooth_alpha(STEP_S, tau));
}

static void
test_half_steps_compose_to_full_step(void) {
    const float time_scale = fixture();
    const float half_left = smooth_decay(STEP_S / 2, time_scale);
    TEST_ASSERT_FLOAT_WITHIN(TOLERANCE, smooth_decay(STEP_S, time_scale), half_left * half_left);
    const float half_alpha = smooth_alpha(STEP_S / 2, time_scale);
    const float composed_alpha = half_alpha + (1.0F - half_alpha) * half_alpha;
    TEST_ASSERT_FLOAT_WITHIN(TOLERANCE, smooth_alpha(STEP_S, time_scale), composed_alpha);
}

void
run_smooth_suite(void) {
    RUN_TEST(test_one_half_life_leaves_half);
    RUN_TEST(test_alpha_at_zero_and_one_time_constant);
    RUN_TEST(test_time_constant_matches_half_life);
    RUN_TEST(test_half_steps_compose_to_full_step);
}
SUITE_REGISTER(run_smooth_suite)

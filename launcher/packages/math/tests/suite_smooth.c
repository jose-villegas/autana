#include <math.h>

#include "math/scalar/math_const.h"
#include "math/signal/smooth.h"
#include "testkit/suite.h"
#include "unity.h"

#define HALF_LIFE_S                   0.35F
#define TAU_S                         0.35F
#define STEP_S                        0.5F
#define TOLERANCE                     1.0E-6F
#define SMALL_STEP_RELATIVE_TOLERANCE 1.0E-4F

static void
test_one_half_life_leaves_half(void) {
    TEST_ASSERT_FLOAT_WITHIN(TOLERANCE, 0.5F, smooth_decay(HALF_LIFE_S, HALF_LIFE_S));
    TEST_ASSERT_EQUAL_FLOAT(1.0F, smooth_decay(0.0F, HALF_LIFE_S));
}

static void
test_alpha_at_zero_and_one_time_constant(void) {
    TEST_ASSERT_EQUAL_FLOAT(0.0F, smooth_alpha(0.0F, TAU_S));
    TEST_ASSERT_FLOAT_WITHIN(TOLERANCE, 1.0F - expf(-1.0F), smooth_alpha(TAU_S, TAU_S));
}

static void
test_time_constant_matches_half_life(void) {
    const float tau = HALF_LIFE_S / MATH_LN2;
    TEST_ASSERT_FLOAT_WITHIN(TOLERANCE, 1.0F - smooth_decay(STEP_S, HALF_LIFE_S), smooth_alpha(STEP_S, tau));
}

static void
test_half_steps_compose_to_full_step(void) {
    const float half_left = smooth_decay(STEP_S / 2, HALF_LIFE_S);
    TEST_ASSERT_FLOAT_WITHIN(TOLERANCE, smooth_decay(STEP_S, HALF_LIFE_S), half_left * half_left);
    const float half_alpha = smooth_alpha(STEP_S / 2, TAU_S);
    const float composed_alpha = half_alpha + (1.0F - half_alpha) * half_alpha;
    TEST_ASSERT_FLOAT_WITHIN(TOLERANCE, smooth_alpha(STEP_S, TAU_S), composed_alpha);
}

static void
test_alpha_keeps_small_steps_exact(void) {
    static const float ratios[] = {1.0E-4F, 1.0E-5F, 1.0E-6F, 1.0E-7F, 1.0E-8F};
    for (size_t i = 0; i < sizeof ratios / sizeof ratios[0]; ++i) {
        const float alpha = smooth_alpha(ratios[i] * TAU_S, TAU_S);
        TEST_ASSERT_TRUE(alpha > 0.0F);
        TEST_ASSERT_FLOAT_WITHIN(ratios[i] * SMALL_STEP_RELATIVE_TOLERANCE, ratios[i], alpha);
    }
}

void
suite_smooth(void) {
    RUN_TEST(test_one_half_life_leaves_half);
    RUN_TEST(test_alpha_at_zero_and_one_time_constant);
    RUN_TEST(test_time_constant_matches_half_life);
    RUN_TEST(test_half_steps_compose_to_full_step);
    RUN_TEST(test_alpha_keeps_small_steps_exact);
}
SUITE_REGISTER(suite_smooth)

/*
 * Portable suite: follow.h, exact steps toward a target, and above all that
 * a step is the same whether it is taken whole or in pieces.
 */

#include "testkit/suite.h"
#include "unity.h"

#include "kinematics/follow/follow.h"

/* This suite's own fixture: a step, its pieces, and a time scale. */
#define STEP_S      0.5F
#define PIECES      50
#define HALF_LIFE_S 0.35F
#define TIME_S      0.12F
#define TOLERANCE   1.0E-5F

static void
test_one_half_life_leaves_half(void) {
    TEST_ASSERT_FLOAT_WITHIN(TOLERANCE, 0.5F, follow_halved_over(HALF_LIFE_S, HALF_LIFE_S));
    TEST_ASSERT_EQUAL_FLOAT(1.0F, follow_halved_over(0.0F, HALF_LIFE_S));
}

static void
test_a_coast_whole_or_in_pieces_ends_alike_short_of_its_limit(void) {
    float x = 0.0F, v = 1.0F, xs = 0.0F, vs = 1.0F;
    follow_coast(&x, &v, STEP_S, HALF_LIFE_S);
    for (int i = 0; i < PIECES; i++) {
        follow_coast(&xs, &vs, STEP_S / PIECES, HALF_LIFE_S);
    }
    TEST_ASSERT_FLOAT_WITHIN(TOLERANCE, x, xs);
    TEST_ASSERT_FLOAT_WITHIN(TOLERANCE, v, vs);
    TEST_ASSERT_TRUE(x < HALF_LIFE_S / MATH_LN2);
}

static void
test_a_spring_whole_or_in_pieces_ends_alike_and_from_rest_never_crosses(void) {
    float e = 1.0F, v = 0.0F, es = 1.0F, vs = 0.0F;
    follow_spring(&e, &v, STEP_S, TIME_S);
    for (int i = 0; i < PIECES; i++) {
        follow_spring(&es, &vs, STEP_S / PIECES, TIME_S);
        TEST_ASSERT_TRUE(es > 0.0F);
    }
    TEST_ASSERT_FLOAT_WITHIN(TOLERANCE, e, es);
    TEST_ASSERT_FLOAT_WITHIN(TOLERANCE, v, vs);
}

void
suite_follow(void) {
    RUN_TEST(test_one_half_life_leaves_half);
    RUN_TEST(test_a_coast_whole_or_in_pieces_ends_alike_short_of_its_limit);
    RUN_TEST(test_a_spring_whole_or_in_pieces_ends_alike_and_from_rest_never_crosses);
}

SUITE_REGISTER(suite_follow);

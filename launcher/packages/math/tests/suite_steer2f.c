#include <math.h>

#include "math/motion/steer2f.h"
#include "math/scalar/math_const.h"
#include "testkit/suite.h"
#include "unity.h"

#define STEP_S        0.02F
#define SETTLE_STEPS  2000
#define TOLERANCE     1.0E-4F
/* Arrived: inside this share of arrive_radius, slower than this share of max_speed. */
#define ARRIVED_SHARE 0.05F

static const steer2f_limits_t LIMITS = {
    .max_speed = 2.0F,
    .max_accel = 3.0F,
    .turn_rate = MATH_PI,
    .arrive_radius = 1.0F,
    .separation_radius = 1.2F,
    .heading_speed = 0.05F,
    .horizon_s = 1.0F,
};

static float
angle_between(float a, float b) {
    return fabsf(remainderf(a - b, MATH_TAU));
}

static void
test_arrive_from_rest_asks_for_the_full_speed_toward_the_target(void) {
    const steer2f_agent_t a = {{0.0F, 0.0F}, {0.0F, 0.0F}, 0.0F};
    const vec2f_t accel = steer2f_arrive(&a, (vec2f_t){0.0F, 5.0F}, LIMITS.max_speed, &LIMITS);
    TEST_ASSERT_FLOAT_WITHIN(TOLERANCE, 0.0F, accel.x);
    TEST_ASSERT_FLOAT_WITHIN(TOLERANCE, LIMITS.max_speed, accel.y);
}

static void
test_arrive_while_moving_away_is_clamped_to_the_accel_limit(void) {
    const steer2f_agent_t a = {{0.0F, 0.0F}, {0.0F, -LIMITS.max_speed}, 0.0F};
    const vec2f_t accel = steer2f_arrive(&a, (vec2f_t){0.0F, 5.0F}, LIMITS.max_speed, &LIMITS);
    TEST_ASSERT_FLOAT_WITHIN(TOLERANCE, 0.0F, accel.x);
    TEST_ASSERT_FLOAT_WITHIN(TOLERANCE, LIMITS.max_accel, accel.y);
}

static void
test_arrive_stops_on_the_target(void) {
    steer2f_agent_t a = {{-3.0F, 2.0F}, {1.0F, -1.0F}, 0.0F};
    const vec2f_t target = {4.0F, -1.0F};
    for (int i = 0; i < SETTLE_STEPS; i++) {
        steer2f_step(&a, steer2f_arrive(&a, target, LIMITS.max_speed, &LIMITS), STEP_S, &LIMITS);
    }
    TEST_ASSERT_TRUE(vec2f_length(vec2f_sub(a.position, target)) < ARRIVED_SHARE * LIMITS.arrive_radius);
    TEST_ASSERT_TRUE(vec2f_length(a.velocity) < ARRIVED_SHARE * LIMITS.max_speed);
}

static void
test_separation_pushes_near_agents_apart_and_ignores_far_ones(void) {
    const steer2f_agent_t agents[] = {
        {{0.0F, 0.0F}, {0.0F, 0.0F}, 0.0F},
        {{0.5F, 0.0F}, {0.0F, 0.0F}, 0.0F},
        {{0.0F, 3.0F}, {0.0F, 0.0F}, 0.0F},
    };
    const vec2f_t first = steer2f_separation(agents, 3, 0, &LIMITS);
    const vec2f_t second = steer2f_separation(agents, 3, 1, &LIMITS);
    TEST_ASSERT_TRUE(first.x < 0.0F);
    TEST_ASSERT_TRUE(second.x > 0.0F);
    TEST_ASSERT_FLOAT_WITHIN(TOLERANCE, 0.0F, first.y);
    TEST_ASSERT_TRUE(vec2f_length(first) <= LIMITS.max_accel + TOLERANCE);
    const vec2f_t lone = steer2f_separation(agents, 3, 2, &LIMITS);
    TEST_ASSERT_FLOAT_WITHIN(TOLERANCE, 0.0F, vec2f_length(lone));
}

static void
test_separation_is_stronger_the_nearer_the_neighbour(void) {
    const steer2f_agent_t near[] = {{{0.0F, 0.0F}, {0.0F, 0.0F}, 0.0F}, {{0.3F, 0.0F}, {0.0F, 0.0F}, 0.0F}};
    const steer2f_agent_t far[] = {{{0.0F, 0.0F}, {0.0F, 0.0F}, 0.0F}, {{0.9F, 0.0F}, {0.0F, 0.0F}, 0.0F}};
    TEST_ASSERT_TRUE(vec2f_length(steer2f_separation(near, 2, 0, &LIMITS))
                     > vec2f_length(steer2f_separation(far, 2, 0, &LIMITS)));
}

static void
test_avoid_pushes_closing_agents_apart_before_they_meet(void) {
    /* Head on along x, 0.4 m apart in y, 3 m apart in x, closing at 4 m/s: closest in 0.75 s, 0.4 m apart. */
    const steer2f_agent_t agents[] = {
        {{-1.5F, 0.2F}, {2.0F, 0.0F}, 0.0F},
        {{1.5F, -0.2F}, {-2.0F, 0.0F}, MATH_PI},
    };
    TEST_ASSERT_FLOAT_WITHIN(TOLERANCE, 0.0F, vec2f_length(steer2f_separation(agents, 2, 0, &LIMITS)));
    const vec2f_t first = steer2f_avoid(agents, 2, 0, &LIMITS);
    const vec2f_t second = steer2f_avoid(agents, 2, 1, &LIMITS);
    TEST_ASSERT_TRUE(first.y > 0.0F);
    TEST_ASSERT_TRUE(second.y < 0.0F);
    TEST_ASSERT_TRUE(vec2f_length(first) <= LIMITS.max_accel + TOLERANCE);
}

/* Exactly head on, nothing tells them which way to go: they still part opposite ways. */
static void
test_avoid_parts_a_dead_on_pair_opposite_ways(void) {
    const steer2f_agent_t agents[] = {
        {{-1.5F, 0.0F}, {2.0F, 0.0F}, 0.0F},
        {{1.5F, 0.0F}, {-2.0F, 0.0F}, MATH_PI},
    };
    const vec2f_t first = steer2f_avoid(agents, 2, 0, &LIMITS);
    const vec2f_t second = steer2f_avoid(agents, 2, 1, &LIMITS);
    TEST_ASSERT_TRUE(vec2f_length(first) > TOLERANCE);
    TEST_ASSERT_TRUE(vec2f_dot(first, second) < 0.0F);
}

static void
test_avoid_ignores_parting_far_and_late_agents(void) {
    const steer2f_agent_t parting[] = {{{0.0F, 0.0F}, {-1.0F, 0.0F}, 0.0F}, {{1.0F, 0.0F}, {1.0F, 0.0F}, 0.0F}};
    TEST_ASSERT_FLOAT_WITHIN(TOLERANCE, 0.0F, vec2f_length(steer2f_avoid(parting, 2, 0, &LIMITS)));
    const steer2f_agent_t wide[] = {{{-2.0F, 0.0F}, {1.0F, 0.0F}, 0.0F}, {{2.0F, 3.0F}, {-1.0F, 0.0F}, 0.0F}};
    TEST_ASSERT_FLOAT_WITHIN(TOLERANCE, 0.0F, vec2f_length(steer2f_avoid(wide, 2, 0, &LIMITS)));
    /* Would meet, but past the horizon. */
    const steer2f_agent_t late[] = {{{-5.0F, 0.0F}, {1.0F, 0.0F}, 0.0F}, {{5.0F, 0.0F}, {-1.0F, 0.0F}, 0.0F}};
    TEST_ASSERT_FLOAT_WITHIN(TOLERANCE, 0.0F, vec2f_length(steer2f_avoid(late, 2, 0, &LIMITS)));
}

static void
test_contain_is_zero_inside_and_turns_back_outside(void) {
    const vec2f_t centre = vec2f_one();
    const float radius = 4.0F;
    const steer2f_agent_t inside = {{3.0F, 1.0F}, {0.0F, 0.0F}, 0.0F};
    TEST_ASSERT_FLOAT_WITHIN(TOLERANCE, 0.0F, vec2f_length(steer2f_contain(&inside, centre, radius, &LIMITS)));
    const steer2f_agent_t just_out = {{1.0F + radius + (0.5F * LIMITS.arrive_radius), 1.0F}, {0.0F, 0.0F}, 0.0F};
    const vec2f_t back = steer2f_contain(&just_out, centre, radius, &LIMITS);
    TEST_ASSERT_FLOAT_WITHIN(TOLERANCE, -0.5F * LIMITS.max_accel, back.x);
    TEST_ASSERT_FLOAT_WITHIN(TOLERANCE, 0.0F, back.y);
    const steer2f_agent_t far_out = {{1.0F, 1.0F - radius - (3.0F * LIMITS.arrive_radius)}, {0.0F, 0.0F}, 0.0F};
    const vec2f_t home = steer2f_contain(&far_out, centre, radius, &LIMITS);
    TEST_ASSERT_FLOAT_WITHIN(TOLERANCE, LIMITS.max_accel, home.y);
}

static void
test_step_holds_speed_and_turn_rate(void) {
    steer2f_agent_t a = {{0.0F, 0.0F}, {0.0F, 0.0F}, 0.0F};
    const vec2f_t push = {-100.0F, 0.0F}; /* far past max_accel, straight behind the heading */
    for (int i = 0; i < SETTLE_STEPS; i++) {
        const float before = a.heading;
        steer2f_step(&a, push, STEP_S, &LIMITS);
        TEST_ASSERT_TRUE(vec2f_length(a.velocity) <= LIMITS.max_speed + TOLERANCE);
        TEST_ASSERT_TRUE(angle_between(a.heading, before) <= LIMITS.turn_rate * STEP_S + TOLERANCE);
    }
    TEST_ASSERT_FLOAT_WITHIN(TOLERANCE, LIMITS.max_speed, vec2f_length(a.velocity));
    TEST_ASSERT_FLOAT_WITHIN(TOLERANCE, 0.0F, angle_between(a.heading, MATH_PI));
}

static void
test_step_moves_by_velocity_and_a_still_agent_keeps_its_heading(void) {
    steer2f_agent_t a = {{1.0F, 1.0F}, {0.0F, 0.0F}, 1.0F};
    steer2f_step(&a, vec2f_zero(), STEP_S, &LIMITS);
    TEST_ASSERT_EQUAL_FLOAT(1.0F, a.heading);
    TEST_ASSERT_EQUAL_FLOAT(1.0F, a.position.x);
    steer2f_agent_t b = {{0.0F, 0.0F}, {1.0F, 0.0F}, 0.0F};
    steer2f_step(&b, vec2f_zero(), STEP_S, &LIMITS);
    TEST_ASSERT_FLOAT_WITHIN(TOLERANCE, STEP_S, b.position.x);
}

void
suite_steer2f(void) {
    RUN_TEST(test_arrive_from_rest_asks_for_the_full_speed_toward_the_target);
    RUN_TEST(test_arrive_while_moving_away_is_clamped_to_the_accel_limit);
    RUN_TEST(test_arrive_stops_on_the_target);
    RUN_TEST(test_separation_pushes_near_agents_apart_and_ignores_far_ones);
    RUN_TEST(test_separation_is_stronger_the_nearer_the_neighbour);
    RUN_TEST(test_avoid_pushes_closing_agents_apart_before_they_meet);
    RUN_TEST(test_avoid_parts_a_dead_on_pair_opposite_ways);
    RUN_TEST(test_avoid_ignores_parting_far_and_late_agents);
    RUN_TEST(test_contain_is_zero_inside_and_turns_back_outside);
    RUN_TEST(test_step_holds_speed_and_turn_rate);
    RUN_TEST(test_step_moves_by_velocity_and_a_still_agent_keeps_its_heading);
}
SUITE_REGISTER(suite_steer2f)

/*
 * Portable suite: fly_motion.h. The keys move the transform along its own
 * right and forward and the world's up at its speed, a drag turns it with a
 * level horizon, and the wheel scales its speed within bounds.
 */

#include "testkit/suite.h"
#include "unity.h"

#include "kinematics/fly/fly_motion.h"

#include <math.h>

#define TOLERANCE    1.0E-4F
#define STEP_S       0.5F
#define QUARTER_TURN (MATH_TAU / 4.0F)
/* A drag that turns a quarter turn. */
#define QUARTER_DRAG (QUARTER_TURN / FLY_MOTION_RADIANS_PER_PIXEL)
/* Far enough either way to reach a wheel bound from the start speed. */
#define FAR_NOTCHES  1000.0F
/* A start speed for a scene in centimetres. */
#define SCENE_SPEED  150.0F

static void
expect_near(vec3f_t expected, vec3f_t actual) {
    TEST_ASSERT_FLOAT_WITHIN(TOLERANCE, expected.x, actual.x);
    TEST_ASSERT_FLOAT_WITHIN(TOLERANCE, expected.y, actual.y);
    TEST_ASSERT_FLOAT_WITHIN(TOLERANCE, expected.z, actual.z);
}

static vec3f_t
forward_of(const fly_motion_t* fly) {
    return quatf_rotate(fly->pose.rotation, vec3f_forward());
}

/* At the origin, facing `direction` with the world's up above. */
static fly_motion_t
facing(vec3f_t direction) {
    return fly_motion_start(transformf_looking(vec3f_zero(), direction, vec3f_up()), FLY_MOTION_START_SPEED);
}

static void
test_no_input_or_no_time_leaves_the_pose(void) {
    fly_motion_t fly = facing(vec3f_forward());
    const fly_motion_steer_t none = {0};
    TEST_ASSERT_FALSE(fly_motion_update(&fly, &none, 1.0F));
    expect_near(vec3f_zero(), fly.pose.position);
    expect_near(vec3f_forward(), forward_of(&fly));
    const fly_motion_steer_t keys = {.move = vec3f_forward()};
    TEST_ASSERT_FALSE(fly_motion_update(&fly, &keys, 0.0F));
}

static void
test_forward_moves_speed_times_dt_along_the_pose_forward(void) {
    /* Facing +x: forward is +x. */
    fly_motion_t fly = facing(vec3f_right());
    const fly_motion_steer_t keys = {.move = vec3f_forward()};
    TEST_ASSERT_TRUE(fly_motion_update(&fly, &keys, STEP_S));
    expect_near(vec3f_scale(vec3f_right(), FLY_MOTION_START_SPEED * STEP_S), fly.pose.position);
}

static void
test_right_moves_along_the_pose_right(void) {
    /* Facing +x, the pose's right is -z. */
    fly_motion_t fly = facing(vec3f_right());
    const fly_motion_steer_t keys = {.move = vec3f_right()};
    (void)fly_motion_update(&fly, &keys, STEP_S);
    expect_near(vec3f_scale(vec3f_unit_z(), -FLY_MOTION_START_SPEED * STEP_S), fly.pose.position);
}

static void
test_up_climbs_the_world_up_even_looking_down_a_slant(void) {
    fly_motion_t fly = facing(vec3f_normalize((vec3f_t){0.0F, -1.0F, 1.0F}));
    const fly_motion_steer_t keys = {.move = vec3f_up()};
    (void)fly_motion_update(&fly, &keys, STEP_S);
    expect_near(vec3f_scale(vec3f_up(), FLY_MOTION_START_SPEED * STEP_S), fly.pose.position);
}

static void
test_a_diagonal_is_no_faster(void) {
    fly_motion_t fly = facing(vec3f_forward());
    const fly_motion_steer_t keys = {.move = {1.0F, 0.0F, 1.0F}};
    (void)fly_motion_update(&fly, &keys, STEP_S);
    const vec3f_t at = fly.pose.position;
    TEST_ASSERT_FLOAT_WITHIN(TOLERANCE, FLY_MOTION_START_SPEED * STEP_S, sqrtf(vec3f_dot(at, at)));
}

static void
test_a_quarter_drag_right_turns_to_face_right_with_a_level_horizon(void) {
    fly_motion_t fly = facing(vec3f_forward());
    const fly_motion_steer_t drag = {.look = {QUARTER_DRAG, 0.0F}};
    TEST_ASSERT_TRUE(fly_motion_update(&fly, &drag, 0.0F));
    expect_near(vec3f_right(), forward_of(&fly));
    expect_near(vec3f_up(), quatf_rotate(fly.pose.rotation, vec3f_up()));
}

static void
test_dragging_far_down_stops_at_the_pitch_limit(void) {
    fly_motion_t fly = facing(vec3f_forward());
    const fly_motion_steer_t drag = {.look = {0.0F, 4.0F * QUARTER_DRAG}};
    (void)fly_motion_update(&fly, &drag, 0.0F);
    TEST_ASSERT_FLOAT_WITHIN(TOLERANCE, -sinf(FLY_MOTION_PITCH_LIMIT), forward_of(&fly).y);
}

/* Straight down with the picture's top towards +x, as no straight-ahead
 * heading would guess: looking up from it tips forward towards the top. */
static void
test_looking_up_from_straight_down_tips_towards_the_picture_top(void) {
    fly_motion_t fly =
        fly_motion_start(transformf_looking(vec3f_zero(), vec3f_down(), vec3f_right()), FLY_MOTION_START_SPEED);
    const fly_motion_steer_t drag = {.look = {0.0F, -QUARTER_DRAG / 2.0F}};
    (void)fly_motion_update(&fly, &drag, 0.0F);
    const vec3f_t tipped = forward_of(&fly);
    TEST_ASSERT_TRUE(tipped.x > 0.0F);
    TEST_ASSERT_FLOAT_WITHIN(TOLERANCE, 0.0F, tipped.z);
}

static void
test_the_wheel_scales_the_speed_within_bounds_and_leaves_the_pose(void) {
    /* Any start speed: the bounds are its multiples. */
    fly_motion_t fly = fly_motion_start(transformf_identity(), SCENE_SPEED);
    fly_motion_steer_t notch = {.wheel = 1.0F};
    TEST_ASSERT_FALSE(fly_motion_update(&fly, &notch, 1.0F));
    TEST_ASSERT_FLOAT_WITHIN(TOLERANCE, SCENE_SPEED * FLY_MOTION_SPEED_STEP, fly.speed);
    notch.wheel = -FAR_NOTCHES;
    (void)fly_motion_update(&fly, &notch, 0.0F);
    TEST_ASSERT_EQUAL_FLOAT(SCENE_SPEED * FLY_MOTION_SPEED_MIN_RATIO, fly.speed);
    notch.wheel = FAR_NOTCHES;
    (void)fly_motion_update(&fly, &notch, 0.0F);
    TEST_ASSERT_EQUAL_FLOAT(SCENE_SPEED * FLY_MOTION_SPEED_MAX_RATIO, fly.speed);
}

void
suite_fly_motion(void) {
    RUN_TEST(test_no_input_or_no_time_leaves_the_pose);
    RUN_TEST(test_forward_moves_speed_times_dt_along_the_pose_forward);
    RUN_TEST(test_right_moves_along_the_pose_right);
    RUN_TEST(test_up_climbs_the_world_up_even_looking_down_a_slant);
    RUN_TEST(test_a_diagonal_is_no_faster);
    RUN_TEST(test_a_quarter_drag_right_turns_to_face_right_with_a_level_horizon);
    RUN_TEST(test_dragging_far_down_stops_at_the_pitch_limit);
    RUN_TEST(test_looking_up_from_straight_down_tips_towards_the_picture_top);
    RUN_TEST(test_the_wheel_scales_the_speed_within_bounds_and_leaves_the_pose);
}

SUITE_REGISTER(suite_fly_motion);

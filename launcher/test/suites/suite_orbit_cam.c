/*
 * orbit_cam: the fit keeps a sphere inside the padded picture from every
 * angle, through the raster's own lens; flings and resets move the same at
 * any frame rate; a reset settles at home the short way round without
 * overshooting; the limits stop what runs into them.
 */

#include <math.h>

#include "gfx/gfx.h"
#include "render/camera.h"
#include "render/r3d_pipeline.h"
#include "scene/camera/orbit_cam.h"
#include "suites.h"
#include "unity.h"

#define LENS_TAN      0.5F
#define SPHERE_RADIUS 0.8F
#define SPHERE_POINTS 2048 /* spaced finely enough that the nearest sample to the rim projects within a pixel of it */
#define PIXEL_SLACK   1.0F
#define YAW_STEPS     24
#define PITCH_STEPS   9
#define NEAR_Z        0.05F
#define DRAG_FRAMES   10
#define STILL_FRAMES  30 /* half a second: the drag speed has halved some 16 times */
#define FRAME_S       (1.0F / 60.0F)
#define DRAG_TURN     0.02F /* radians a frame */
#define FLING_S       0.5F
#define FINE_STEPS    50
#define STATE_EPSILON 1.0E-4F
#define SETTLE_S      2.0F
#define DEGREES       (MATH_PI / 180.0F)

static const vec3f_t TARGET = {0.0F, 0.4F, 0.2F};

static orbit_limits_t
open_limits(float log_dist) {
    return (orbit_limits_t){-ORBIT_PITCH_LIMIT, ORBIT_PITCH_LIMIT, log_dist, log_dist};
}

static orbit_cam_t
cam_at(float yaw, float pitch) {
    const float log_dist = logf(orbit_fit_distance(SPHERE_RADIUS, LENS_TAN, ORBIT_FRAME_PADDING));
    orbit_cam_t cam;
    orbit_init(&cam, TARGET, (orbit_state_t){yaw, pitch, log_dist}, open_limits(log_dist));
    return cam;
}

static camera_t
camera_of(const orbit_cam_t* cam) {
    const transformf_t pose = orbit_pose(cam);
    return (camera_t){pose.position, quatf_rotate(pose.rotation, (vec3f_t){0.0F, 0.0F, 1.0F}), LENS_TAN, NEAR_Z};
}

/* The i-th of n points spread evenly over the unit sphere (a Fibonacci spiral). */
static vec3f_t
sphere_point(int i, int n) {
    const float golden = MATH_PI * (3.0F - sqrtf(5.0F));
    const float y = 1.0F - (2.0F * ((float)i + 0.5F) / (float)n);
    const float ring = sqrtf(1.0F - (y * y));
    return (vec3f_t){ring * cosf(golden * (float)i), y, ring * sinf(golden * (float)i)};
}

/* The furthest any point of the sphere lands from the picture's centre, in
 * pixels along each axis. */
static void
sphere_reach(const camera_t* camera, viewport_t viewport, float* reach_x, float* reach_y) {
    r3d_lens_t lens;
    r3d_lens_init(&lens, camera, 1, viewport);
    *reach_x = 0.0F;
    *reach_y = 0.0F;
    for (int i = 0; i < SPHERE_POINTS; i++) {
        const vec3f_t p = vec3f_add(TARGET, vec3f_scale(sphere_point(i, SPHERE_POINTS), SPHERE_RADIUS));
        float q[3];
        for (int r = 0; r < 3; r++) {
            q[r] = (lens.m[r][0] * p.x) + (lens.m[r][1] * p.y) + (lens.m[r][2] * p.z) + lens.m[r][3];
        }
        TEST_ASSERT_TRUE_MESSAGE(q[2] > NEAR_Z, "the whole sphere is in front of the near plane");
        *reach_x = fmaxf(*reach_x, fabsf(q[0] / q[2]));
        *reach_y = fmaxf(*reach_y, fabsf(q[1] / q[2]));
    }
}

static void
check_fit_in(viewport_t viewport) {
    const float half_w = (float)viewport.width * 0.5F;
    const float half_h = (float)viewport.height * 0.5F;
    const float short_half = fminf(half_w, half_h);
    const float clear = 1.0F - ORBIT_FRAME_PADDING;
    for (int iy = 0; iy < YAW_STEPS; iy++) {
        for (int ip = 0; ip < PITCH_STEPS; ip++) {
            const float yaw = MATH_TAU * (float)iy / (float)YAW_STEPS;
            const float pitch = -ORBIT_PITCH_LIMIT + (2.0F * ORBIT_PITCH_LIMIT * (float)ip / (float)(PITCH_STEPS - 1));
            const orbit_cam_t cam = cam_at(yaw, pitch);
            const camera_t camera = camera_of(&cam);
            float reach_x, reach_y;
            sphere_reach(&camera, viewport, &reach_x, &reach_y);
            TEST_ASSERT_LESS_OR_EQUAL_FLOAT(clear * half_w + PIXEL_SLACK, reach_x);
            TEST_ASSERT_LESS_OR_EQUAL_FLOAT(clear * half_h + PIXEL_SLACK, reach_y);
            /* Not over-padded: the shorter axis's padded edge is touched. */
            TEST_ASSERT_GREATER_OR_EQUAL_FLOAT(clear * short_half - PIXEL_SLACK, fmaxf(reach_x, reach_y));
        }
    }
}

void
test_orbit_fit_keeps_the_sphere_inside_the_padded_portrait_picture_from_every_angle(void) {
    check_fit_in((viewport_t){GFX_WIDTH, GFX_HEIGHT, 0});
}

void
test_orbit_fit_lets_the_shorter_axis_bind_when_the_picture_is_held_landscape(void) {
    check_fit_in((viewport_t){GFX_HEIGHT, GFX_WIDTH, 0});
}

void
test_orbit_pose_stands_at_the_distance_looking_at_the_target_and_level(void) {
    const orbit_cam_t cam = cam_at(40.0F * DEGREES, 25.0F * DEGREES);
    const transformf_t pose = orbit_pose(&cam);
    const vec3f_t to_target = vec3f_sub(TARGET, pose.position);
    const float d = sqrtf(vec3f_dot(to_target, to_target));
    TEST_ASSERT_FLOAT_WITHIN(STATE_EPSILON, expf(cam.at.log_dist), d);
    const vec3f_t forward = quatf_rotate(pose.rotation, (vec3f_t){0.0F, 0.0F, 1.0F});
    TEST_ASSERT_FLOAT_WITHIN(STATE_EPSILON, 1.0F, vec3f_dot(forward, vec3f_scale(to_target, 1.0F / d)));
    TEST_ASSERT_FLOAT_WITHIN(STATE_EPSILON, 0.0F, quatf_rotate(pose.rotation, (vec3f_t){1.0F, 0.0F, 0.0F}).y);
    TEST_ASSERT_TRUE_MESSAGE(pose.position.y > TARGET.y, "a positive pitch puts the eye above the target");
}

static void
drag(orbit_cam_t* cam, float yaw_turn, float pitch_turn, int frames) {
    const orbit_input_t input = {yaw_turn, pitch_turn, true, false};
    for (int i = 0; i < frames; i++) {
        orbit_update(cam, &input, FRAME_S);
    }
}

static void
coast(orbit_cam_t* cam, float seconds, int steps) {
    const orbit_input_t idle = {0};
    for (int i = 0; i < steps; i++) {
        orbit_update(cam, &idle, seconds / (float)steps);
    }
}

static void
assert_same_state(const orbit_cam_t* a, const orbit_cam_t* b) {
    TEST_ASSERT_FLOAT_WITHIN(STATE_EPSILON, a->at.yaw, b->at.yaw);
    TEST_ASSERT_FLOAT_WITHIN(STATE_EPSILON, a->at.pitch, b->at.pitch);
    TEST_ASSERT_FLOAT_WITHIN(STATE_EPSILON, a->velocity.yaw, b->velocity.yaw);
    TEST_ASSERT_FLOAT_WITHIN(STATE_EPSILON, a->velocity.pitch, b->velocity.pitch);
}

void
test_orbit_a_drag_turns_by_the_finger_and_a_lift_keeps_turning_the_same_way(void) {
    orbit_cam_t cam = cam_at(0.0F, 0.0F);
    drag(&cam, DRAG_TURN, 0.0F, DRAG_FRAMES);
    TEST_ASSERT_FLOAT_WITHIN(STATE_EPSILON, DRAG_TURN * DRAG_FRAMES, cam.at.yaw);
    const float lifted_at = cam.at.yaw;
    coast(&cam, FLING_S, 1);
    TEST_ASSERT_TRUE(cam.at.yaw > lifted_at);
    TEST_ASSERT_TRUE(cam.velocity.yaw < DRAG_TURN / FRAME_S);
}

void
test_orbit_a_fling_moves_the_same_in_one_step_as_in_many(void) {
    orbit_cam_t one = cam_at(0.0F, 0.0F);
    drag(&one, DRAG_TURN, DRAG_TURN * 0.5F, DRAG_FRAMES);
    orbit_cam_t many = one;
    coast(&one, FLING_S, 1);
    coast(&many, FLING_S, FINE_STEPS);
    assert_same_state(&one, &many);
}

void
test_orbit_a_finger_held_still_before_lifting_flings_nothing(void) {
    orbit_cam_t cam = cam_at(0.0F, 0.0F);
    drag(&cam, DRAG_TURN, 0.0F, DRAG_FRAMES);
    drag(&cam, 0.0F, 0.0F, STILL_FRAMES);
    const float lifted_at = cam.at.yaw;
    coast(&cam, SETTLE_S, FINE_STEPS);
    TEST_ASSERT_FLOAT_WITHIN(DRAG_TURN * 0.01F, lifted_at, cam.at.yaw);
}

void
test_orbit_pitch_stops_at_its_limit(void) {
    orbit_cam_t cam = cam_at(0.0F, 0.0F);
    cam.limits.pitch_max = 30.0F * DEGREES;
    drag(&cam, 0.0F, DRAG_TURN * 5.0F, DRAG_FRAMES);
    TEST_ASSERT_EQUAL_FLOAT(cam.limits.pitch_max, cam.at.pitch);
    TEST_ASSERT_EQUAL_FLOAT(0.0F, cam.velocity.pitch);
    coast(&cam, FLING_S, 1);
    TEST_ASSERT_EQUAL_FLOAT(cam.limits.pitch_max, cam.at.pitch);
}

void
test_orbit_init_clamps_home_to_the_limits(void) {
    orbit_cam_t cam;
    orbit_init(&cam, TARGET, (orbit_state_t){0.0F, ORBIT_PITCH_LIMIT, 0.0F}, (orbit_limits_t){0.0F, 0.5F, 0.0F, 0.0F});
    TEST_ASSERT_EQUAL_FLOAT(0.5F, cam.at.pitch);
    TEST_ASSERT_EQUAL_FLOAT(0.5F, cam.home.pitch);
}

void
test_orbit_yaw_stays_within_one_turn_however_far_it_spins(void) {
    orbit_cam_t cam = cam_at(0.0F, 0.0F);
    drag(&cam, MATH_PI * 0.75F, 0.0F, DRAG_FRAMES * 10);
    TEST_ASSERT_TRUE(fabsf(cam.at.yaw) <= MATH_PI);
}

static void
start_reset(orbit_cam_t* cam) {
    const orbit_input_t reset = {0.0F, 0.0F, false, true};
    orbit_update(cam, &reset, 0.0F);
}

void
test_orbit_a_reset_moves_the_same_in_one_step_as_in_many(void) {
    orbit_cam_t one = cam_at(0.0F, 0.0F);
    drag(&one, DRAG_TURN, DRAG_TURN, DRAG_FRAMES);
    start_reset(&one);
    orbit_cam_t many = one;
    coast(&one, ORBIT_RESET_TIME_S, 1);
    coast(&many, ORBIT_RESET_TIME_S, FINE_STEPS);
    assert_same_state(&one, &many);
}

void
test_orbit_a_reset_settles_exactly_at_home_without_overshooting(void) {
    orbit_cam_t cam = cam_at(0.0F, 0.0F);
    drag(&cam, DRAG_TURN, DRAG_TURN, DRAG_FRAMES);
    start_reset(&cam);
    for (int i = 0; i < FINE_STEPS; i++) {
        coast(&cam, SETTLE_S / FINE_STEPS, 1);
        TEST_ASSERT_TRUE_MESSAGE(cam.at.yaw >= cam.home.yaw && cam.at.pitch >= cam.home.pitch, "never past home");
    }
    TEST_ASSERT_FALSE(cam.resetting);
    TEST_ASSERT_EQUAL_FLOAT(cam.home.yaw, cam.at.yaw);
    TEST_ASSERT_EQUAL_FLOAT(cam.home.pitch, cam.at.pitch);
}

void
test_orbit_a_reset_turns_the_short_way_round(void) {
    orbit_cam_t cam = cam_at(0.0F, 0.0F);
    cam.at.yaw = 350.0F * DEGREES; /* 10 degrees short of home going on, 350 coming back */
    start_reset(&cam);
    for (int i = 0; i < FINE_STEPS; i++) {
        coast(&cam, SETTLE_S / FINE_STEPS, 1);
        TEST_ASSERT_TRUE_MESSAGE(fabsf(cam.at.yaw) <= 10.0F * DEGREES + STATE_EPSILON, "never further than 10 degrees");
    }
    TEST_ASSERT_EQUAL_FLOAT(cam.home.yaw, cam.at.yaw);
}

void
test_orbit_a_finger_cancels_a_reset(void) {
    orbit_cam_t cam = cam_at(0.0F, 0.0F);
    drag(&cam, DRAG_TURN, 0.0F, DRAG_FRAMES);
    start_reset(&cam);
    drag(&cam, DRAG_TURN, 0.0F, 1);
    TEST_ASSERT_FALSE(cam.resetting);
}

void
run_orbit_cam_suite(void) {
    RUN_TEST(test_orbit_fit_keeps_the_sphere_inside_the_padded_portrait_picture_from_every_angle);
    RUN_TEST(test_orbit_fit_lets_the_shorter_axis_bind_when_the_picture_is_held_landscape);
    RUN_TEST(test_orbit_pose_stands_at_the_distance_looking_at_the_target_and_level);
    RUN_TEST(test_orbit_a_drag_turns_by_the_finger_and_a_lift_keeps_turning_the_same_way);
    RUN_TEST(test_orbit_a_fling_moves_the_same_in_one_step_as_in_many);
    RUN_TEST(test_orbit_a_finger_held_still_before_lifting_flings_nothing);
    RUN_TEST(test_orbit_pitch_stops_at_its_limit);
    RUN_TEST(test_orbit_init_clamps_home_to_the_limits);
    RUN_TEST(test_orbit_yaw_stays_within_one_turn_however_far_it_spins);
    RUN_TEST(test_orbit_a_reset_moves_the_same_in_one_step_as_in_many);
    RUN_TEST(test_orbit_a_reset_settles_exactly_at_home_without_overshooting);
    RUN_TEST(test_orbit_a_reset_turns_the_short_way_round);
    RUN_TEST(test_orbit_a_finger_cancels_a_reset);
}

SUITE_REGISTER(run_orbit_cam_suite);

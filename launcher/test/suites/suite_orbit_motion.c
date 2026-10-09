/*
 * orbit_motion: a transform it moves keeps facing its target at its
 * distance, whatever entity it belongs to; the camera fit keeps a sphere
 * inside the padded picture from every angle, through the raster's own
 * lens; flings and resets move the same at any frame rate; a reset settles
 * at home the short way round without overshooting; the limits stop what
 * runs into them.
 */

#include <math.h>

#include "gfx/gfx.h"
#include "render/camera.h"
#include "render/r3d_pipeline.h"
#include "render_view_fixture.h"
#include "suites.h"
#include "unity.h"
#include "util/motion/orbit_motion.h"

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

static orbit_motion_limits_t
open_limits(float log_dist) {
    return (orbit_motion_limits_t){-ORBIT_MOTION_PITCH_LIMIT, ORBIT_MOTION_PITCH_LIMIT, log_dist, log_dist};
}

static orbit_motion_t
orbit_at(float yaw, float pitch) {
    const float log_dist = logf(orbit_motion_fit_distance(SPHERE_RADIUS, LENS_TAN, ORBIT_MOTION_FRAME_PADDING));
    orbit_motion_t orbit;
    orbit_motion_init(&orbit, TARGET, (orbit_motion_state_t){yaw, pitch, log_dist}, open_limits(log_dist));
    return orbit;
}

static camera_t
camera_of(const orbit_motion_t* orbit) {
    const transformf_t pose = orbit_motion_pose(orbit);
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
    const render_view_t frame_view = render_view_fixture(camera, viewport);
    r3d_lens_init(&lens, &frame_view, 1);
    *reach_x = 0.0F;
    *reach_y = 0.0F;
    for (int i = 0; i < SPHERE_POINTS; i++) {
        const vec3f_t p = vec3f_add(TARGET, vec3f_scale(sphere_point(i, SPHERE_POINTS), SPHERE_RADIUS));
        const vec3f_t q = mat4f_apply(&lens.m, p);
        TEST_ASSERT_TRUE_MESSAGE(q.z > NEAR_Z, "the whole sphere is in front of the near plane");
        *reach_x = fmaxf(*reach_x, fabsf(q.x / q.z));
        *reach_y = fmaxf(*reach_y, fabsf(q.y / q.z));
    }
}

static void
check_fit_in(viewport_t viewport) {
    const float half_w = (float)viewport.width * 0.5F;
    const float half_h = (float)viewport.height * 0.5F;
    const float short_half = fminf(half_w, half_h);
    const float clear = 1.0F - ORBIT_MOTION_FRAME_PADDING;
    for (int iy = 0; iy < YAW_STEPS; iy++) {
        for (int ip = 0; ip < PITCH_STEPS; ip++) {
            const float yaw = MATH_TAU * (float)iy / (float)YAW_STEPS;
            const float pitch =
                -ORBIT_MOTION_PITCH_LIMIT + (2.0F * ORBIT_MOTION_PITCH_LIMIT * (float)ip / (float)(PITCH_STEPS - 1));
            const orbit_motion_t orbit = orbit_at(yaw, pitch);
            const camera_t camera = camera_of(&orbit);
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
test_orbit_motion_fit_keeps_the_sphere_inside_the_padded_portrait_picture_from_every_angle(void) {
    check_fit_in((viewport_t){GFX_WIDTH, GFX_HEIGHT, 0});
}

void
test_orbit_motion_fit_lets_the_shorter_axis_bind_when_the_picture_is_held_landscape(void) {
    check_fit_in((viewport_t){GFX_HEIGHT, GFX_WIDTH, 0});
}

void
test_orbit_motion_pose_stands_at_the_distance_looking_at_the_target_and_level(void) {
    const orbit_motion_t orbit = orbit_at(40.0F * DEGREES, 25.0F * DEGREES);
    const transformf_t pose = orbit_motion_pose(&orbit);
    const vec3f_t to_target = vec3f_sub(TARGET, pose.position);
    const float d = sqrtf(vec3f_dot(to_target, to_target));
    TEST_ASSERT_FLOAT_WITHIN(STATE_EPSILON, expf(orbit.at.log_dist), d);
    const vec3f_t forward = quatf_rotate(pose.rotation, (vec3f_t){0.0F, 0.0F, 1.0F});
    TEST_ASSERT_FLOAT_WITHIN(STATE_EPSILON, 1.0F, vec3f_dot(forward, vec3f_scale(to_target, 1.0F / d)));
    TEST_ASSERT_FLOAT_WITHIN(STATE_EPSILON, 0.0F, quatf_rotate(pose.rotation, (vec3f_t){1.0F, 0.0F, 0.0F}).y);
    TEST_ASSERT_TRUE_MESSAGE(pose.position.y > TARGET.y, "a positive pitch puts the transform above the target");
}

static void
drag(orbit_motion_t* orbit, float yaw_turn, float pitch_turn, int frames) {
    const orbit_motion_input_t input = {.yaw_turn = yaw_turn, .pitch_turn = pitch_turn, .held = true};
    for (int i = 0; i < frames; i++) {
        orbit_motion_update(orbit, &input, FRAME_S);
    }
}

/* The point `ahead` units along the light's own +z, in the world. */
static vec3f_t
ahead_of(const transformf_t* light, float ahead) {
    const mat4f_t m = transformf_compute_matrix(light);
    return mat4f_apply(&m, (vec3f_t){0.0F, 0.0F, ahead});
}

static void
assert_near(vec3f_t want, vec3f_t got) {
    TEST_ASSERT_FLOAT_WITHIN(STATE_EPSILON, want.x, got.x);
    TEST_ASSERT_FLOAT_WITHIN(STATE_EPSILON, want.y, got.y);
    TEST_ASSERT_FLOAT_WITHIN(STATE_EPSILON, want.z, got.z);
}

void
test_orbit_motion_a_light_orbiting_a_prop_keeps_shining_on_it(void) {
    const vec3f_t prop = {1.0F, 0.0F, -2.0F};
    const float reach = 3.0F;
    orbit_motion_t orbit;
    orbit_motion_init(&orbit, prop, (orbit_motion_state_t){90.0F * DEGREES, 30.0F * DEGREES, logf(reach)},
                      open_limits(logf(reach)));
    transformf_t light = orbit_motion_pose(&orbit);
    assert_near(prop, ahead_of(&light, reach));
    drag(&orbit, DRAG_TURN, DRAG_TURN, DRAG_FRAMES);
    const vec3f_t before = light.position;
    light = orbit_motion_pose(&orbit);
    TEST_ASSERT_FALSE_MESSAGE(vec3f_equal(before, light.position), "the light moved");
    assert_near(prop, ahead_of(&light, reach));
}

static void
coast(orbit_motion_t* orbit, float seconds, int steps) {
    const orbit_motion_input_t idle = {0};
    for (int i = 0; i < steps; i++) {
        orbit_motion_update(orbit, &idle, seconds / (float)steps);
    }
}

static void
assert_same_state(const orbit_motion_t* a, const orbit_motion_t* b) {
    TEST_ASSERT_FLOAT_WITHIN(STATE_EPSILON, a->at.yaw, b->at.yaw);
    TEST_ASSERT_FLOAT_WITHIN(STATE_EPSILON, a->at.pitch, b->at.pitch);
    TEST_ASSERT_FLOAT_WITHIN(STATE_EPSILON, a->velocity.yaw, b->velocity.yaw);
    TEST_ASSERT_FLOAT_WITHIN(STATE_EPSILON, a->velocity.pitch, b->velocity.pitch);
}

void
test_orbit_motion_a_drag_turns_by_the_finger_and_a_lift_keeps_turning_the_same_way(void) {
    orbit_motion_t orbit = orbit_at(0.0F, 0.0F);
    drag(&orbit, DRAG_TURN, 0.0F, DRAG_FRAMES);
    TEST_ASSERT_FLOAT_WITHIN(STATE_EPSILON, DRAG_TURN * DRAG_FRAMES, orbit.at.yaw);
    const float lifted_at = orbit.at.yaw;
    coast(&orbit, FLING_S, 1);
    TEST_ASSERT_TRUE(orbit.at.yaw > lifted_at);
    TEST_ASSERT_TRUE(orbit.velocity.yaw < DRAG_TURN / FRAME_S);
}

void
test_orbit_motion_a_fling_moves_the_same_in_one_step_as_in_many(void) {
    orbit_motion_t one = orbit_at(0.0F, 0.0F);
    drag(&one, DRAG_TURN, DRAG_TURN * 0.5F, DRAG_FRAMES);
    orbit_motion_t many = one;
    coast(&one, FLING_S, 1);
    coast(&many, FLING_S, FINE_STEPS);
    assert_same_state(&one, &many);
}

void
test_orbit_motion_a_finger_held_still_before_lifting_flings_nothing(void) {
    orbit_motion_t orbit = orbit_at(0.0F, 0.0F);
    drag(&orbit, DRAG_TURN, 0.0F, DRAG_FRAMES);
    drag(&orbit, 0.0F, 0.0F, STILL_FRAMES);
    const float lifted_at = orbit.at.yaw;
    coast(&orbit, SETTLE_S, FINE_STEPS);
    TEST_ASSERT_FLOAT_WITHIN(DRAG_TURN * 0.01F, lifted_at, orbit.at.yaw);
}

void
test_orbit_motion_pitch_stops_at_its_limit(void) {
    orbit_motion_t orbit = orbit_at(0.0F, 0.0F);
    orbit.limits.pitch_max = 30.0F * DEGREES;
    drag(&orbit, 0.0F, DRAG_TURN * 5.0F, DRAG_FRAMES);
    TEST_ASSERT_EQUAL_FLOAT(orbit.limits.pitch_max, orbit.at.pitch);
    TEST_ASSERT_EQUAL_FLOAT(0.0F, orbit.velocity.pitch);
    coast(&orbit, FLING_S, 1);
    TEST_ASSERT_EQUAL_FLOAT(orbit.limits.pitch_max, orbit.at.pitch);
}

void
test_orbit_motion_init_clamps_home_to_the_limits(void) {
    orbit_motion_t orbit;
    orbit_motion_init(&orbit, TARGET, (orbit_motion_state_t){0.0F, ORBIT_MOTION_PITCH_LIMIT, 0.0F},
                      (orbit_motion_limits_t){0.0F, 0.5F, 0.0F, 0.0F});
    TEST_ASSERT_EQUAL_FLOAT(0.5F, orbit.at.pitch);
    TEST_ASSERT_EQUAL_FLOAT(0.5F, orbit.home.pitch);
}

void
test_orbit_motion_yaw_stays_within_one_turn_however_far_it_spins(void) {
    orbit_motion_t orbit = orbit_at(0.0F, 0.0F);
    drag(&orbit, MATH_PI * 0.75F, 0.0F, DRAG_FRAMES * 10);
    TEST_ASSERT_TRUE(fabsf(orbit.at.yaw) <= MATH_PI);
}

#define ZOOM_ROOM 0.5F  /* log distance either side of home */
#define ZOOM_TURN 0.02F /* log distance a frame */

/* At home with room to zoom ZOOM_ROOM either way. */
static orbit_motion_t
orbit_with_zoom_room(void) {
    orbit_motion_t orbit = orbit_at(0.0F, 0.0F);
    orbit.limits.log_dist_min = orbit.home.log_dist - ZOOM_ROOM;
    orbit.limits.log_dist_max = orbit.home.log_dist + ZOOM_ROOM;
    return orbit;
}

static void
zoom(orbit_motion_t* orbit, float turn, int frames) {
    const orbit_motion_input_t input = {.held = true, .zoom_turn = turn};
    for (int i = 0; i < frames; i++) {
        orbit_motion_update(orbit, &input, FRAME_S);
    }
}

void
test_orbit_motion_a_held_zoom_moves_the_distance_by_the_finger_without_turning(void) {
    orbit_motion_t orbit = orbit_with_zoom_room();
    const float home = orbit.at.log_dist;
    zoom(&orbit, -ZOOM_TURN, DRAG_FRAMES);
    TEST_ASSERT_FLOAT_WITHIN(STATE_EPSILON, home - ZOOM_TURN * DRAG_FRAMES, orbit.at.log_dist);
    TEST_ASSERT_EQUAL_FLOAT(0.0F, orbit.at.yaw);
    TEST_ASSERT_EQUAL_FLOAT(0.0F, orbit.at.pitch);
}

void
test_orbit_motion_a_zoom_fling_moves_the_same_in_one_step_as_in_many(void) {
    orbit_motion_t one = orbit_with_zoom_room();
    zoom(&one, -ZOOM_TURN, DRAG_FRAMES);
    orbit_motion_t many = one;
    coast(&one, FLING_S, 1);
    coast(&many, FLING_S, FINE_STEPS);
    TEST_ASSERT_TRUE_MESSAGE(one.at.log_dist < many.home.log_dist - ZOOM_TURN * DRAG_FRAMES, "it kept zooming in");
    TEST_ASSERT_FLOAT_WITHIN(STATE_EPSILON, one.at.log_dist, many.at.log_dist);
    TEST_ASSERT_FLOAT_WITHIN(STATE_EPSILON, one.velocity.log_dist, many.velocity.log_dist);
}

void
test_orbit_motion_a_zoom_stops_at_both_limits(void) {
    orbit_motion_t orbit = orbit_with_zoom_room();
    zoom(&orbit, -ZOOM_TURN * 10.0F, DRAG_FRAMES);
    TEST_ASSERT_EQUAL_FLOAT(orbit.limits.log_dist_min, orbit.at.log_dist);
    TEST_ASSERT_EQUAL_FLOAT(0.0F, orbit.velocity.log_dist);
    zoom(&orbit, ZOOM_TURN * 20.0F, DRAG_FRAMES);
    TEST_ASSERT_EQUAL_FLOAT(orbit.limits.log_dist_max, orbit.at.log_dist);
    TEST_ASSERT_EQUAL_FLOAT(0.0F, orbit.velocity.log_dist);
}

void
test_orbit_motion_a_reset_brings_the_distance_home_too(void) {
    orbit_motion_t orbit = orbit_with_zoom_room();
    zoom(&orbit, ZOOM_TURN, DRAG_FRAMES);
    const orbit_motion_input_t reset = {.reset = true};
    orbit_motion_update(&orbit, &reset, 0.0F);
    coast(&orbit, SETTLE_S, FINE_STEPS);
    TEST_ASSERT_FALSE(orbit.resetting);
    TEST_ASSERT_EQUAL_FLOAT(orbit.home.log_dist, orbit.at.log_dist);
}

static void
start_reset(orbit_motion_t* orbit) {
    const orbit_motion_input_t reset = {.reset = true};
    orbit_motion_update(orbit, &reset, 0.0F);
}

void
test_orbit_motion_a_reset_moves_the_same_in_one_step_as_in_many(void) {
    orbit_motion_t one = orbit_at(0.0F, 0.0F);
    drag(&one, DRAG_TURN, DRAG_TURN, DRAG_FRAMES);
    start_reset(&one);
    orbit_motion_t many = one;
    coast(&one, ORBIT_MOTION_RESET_TIME_S, 1);
    coast(&many, ORBIT_MOTION_RESET_TIME_S, FINE_STEPS);
    assert_same_state(&one, &many);
}

void
test_orbit_motion_a_reset_settles_exactly_at_home_without_overshooting(void) {
    orbit_motion_t orbit = orbit_at(0.0F, 0.0F);
    drag(&orbit, DRAG_TURN, DRAG_TURN, DRAG_FRAMES);
    start_reset(&orbit);
    for (int i = 0; i < FINE_STEPS; i++) {
        coast(&orbit, SETTLE_S / FINE_STEPS, 1);
        TEST_ASSERT_TRUE_MESSAGE(orbit.at.yaw >= orbit.home.yaw && orbit.at.pitch >= orbit.home.pitch,
                                 "never past home");
    }
    TEST_ASSERT_FALSE(orbit.resetting);
    TEST_ASSERT_EQUAL_FLOAT(orbit.home.yaw, orbit.at.yaw);
    TEST_ASSERT_EQUAL_FLOAT(orbit.home.pitch, orbit.at.pitch);
}

void
test_orbit_motion_a_reset_turns_the_short_way_round(void) {
    orbit_motion_t orbit = orbit_at(0.0F, 0.0F);
    orbit.at.yaw = 350.0F * DEGREES; /* 10 degrees short of home going on, 350 coming back */
    start_reset(&orbit);
    for (int i = 0; i < FINE_STEPS; i++) {
        coast(&orbit, SETTLE_S / FINE_STEPS, 1);
        TEST_ASSERT_TRUE_MESSAGE(fabsf(orbit.at.yaw) <= 10.0F * DEGREES + STATE_EPSILON,
                                 "never further than 10 degrees");
    }
    TEST_ASSERT_EQUAL_FLOAT(orbit.home.yaw, orbit.at.yaw);
}

void
test_orbit_motion_a_finger_cancels_a_reset(void) {
    orbit_motion_t orbit = orbit_at(0.0F, 0.0F);
    drag(&orbit, DRAG_TURN, 0.0F, DRAG_FRAMES);
    start_reset(&orbit);
    drag(&orbit, DRAG_TURN, 0.0F, 1);
    TEST_ASSERT_FALSE(orbit.resetting);
}

void
run_orbit_motion_suite(void) {
    RUN_TEST(test_orbit_motion_fit_keeps_the_sphere_inside_the_padded_portrait_picture_from_every_angle);
    RUN_TEST(test_orbit_motion_fit_lets_the_shorter_axis_bind_when_the_picture_is_held_landscape);
    RUN_TEST(test_orbit_motion_pose_stands_at_the_distance_looking_at_the_target_and_level);
    RUN_TEST(test_orbit_motion_a_light_orbiting_a_prop_keeps_shining_on_it);
    RUN_TEST(test_orbit_motion_a_drag_turns_by_the_finger_and_a_lift_keeps_turning_the_same_way);
    RUN_TEST(test_orbit_motion_a_fling_moves_the_same_in_one_step_as_in_many);
    RUN_TEST(test_orbit_motion_a_finger_held_still_before_lifting_flings_nothing);
    RUN_TEST(test_orbit_motion_pitch_stops_at_its_limit);
    RUN_TEST(test_orbit_motion_init_clamps_home_to_the_limits);
    RUN_TEST(test_orbit_motion_yaw_stays_within_one_turn_however_far_it_spins);
    RUN_TEST(test_orbit_motion_a_reset_moves_the_same_in_one_step_as_in_many);
    RUN_TEST(test_orbit_motion_a_reset_settles_exactly_at_home_without_overshooting);
    RUN_TEST(test_orbit_motion_a_reset_turns_the_short_way_round);
    RUN_TEST(test_orbit_motion_a_finger_cancels_a_reset);
    RUN_TEST(test_orbit_motion_a_held_zoom_moves_the_distance_by_the_finger_without_turning);
    RUN_TEST(test_orbit_motion_a_zoom_fling_moves_the_same_in_one_step_as_in_many);
    RUN_TEST(test_orbit_motion_a_zoom_stops_at_both_limits);
    RUN_TEST(test_orbit_motion_a_reset_brings_the_distance_home_too);
}

SUITE_REGISTER(run_orbit_motion_suite);

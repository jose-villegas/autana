/*
 * render_lab_fly: a steering line reads whole or not at all (portable), and
 * on the device the lines fly the drawn camera, and every way a flight ends
 * puts the camera back where it began and its clip as it was.
 */
#include "suites.h"

#include "testkit/suite.h"
#include "unity.h"

#include "apps/render_lab/console/render_lab_fly.h"

static void
test_a_steering_line_reads_its_six_numbers(void) {
    fly_motion_steer_t steer;
    TEST_ASSERT_TRUE(render_lab_fly_parse("1 -1 0 -12 7 -2", &steer));
    TEST_ASSERT_EQUAL_FLOAT(1.0F, steer.move.x);
    TEST_ASSERT_EQUAL_FLOAT(-1.0F, steer.move.y);
    TEST_ASSERT_EQUAL_FLOAT(0.0F, steer.move.z);
    TEST_ASSERT_EQUAL_FLOAT(-12.0F, steer.look.x);
    TEST_ASSERT_EQUAL_FLOAT(7.0F, steer.look.y);
    TEST_ASSERT_EQUAL_FLOAT(-2.0F, steer.wheel);
}

static void
test_a_malformed_steering_line_writes_nothing(void) {
    static const char* const bad[] = {
        "",              /* nothing */
        "0 0 1 0 0",     /* one short */
        "0 0 1 0 0 0 0", /* one over */
        "2 0 0 0 0 0",   /* a key axis past 1 */
        "0 -2 0 0 0 0",  /* a key axis past -1 */
        "0 0 1 0.5 0 0", /* a drag in part pixels */
        "0 0 x 0 0 0",   /* not a number */
    };
    const fly_motion_steer_t before = {.wheel = 3.0F};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        fly_motion_steer_t steer = before;
        TEST_ASSERT_FALSE_MESSAGE(render_lab_fly_parse(bad[i], &steer), bad[i]);
        TEST_ASSERT_EQUAL_FLOAT(before.wheel, steer.wheel);
    }
}

static void
test_a_flight_crosses_its_scene_in_the_same_time_whatever_its_units(void) {
    const box3f_t metres = {{0.0F, 0.0F, 0.0F}, {3.0F, 4.0F, 12.0F}}; /* 13 corner to corner */
    const box3f_t centimetres = {vec3f_scale(metres.lo, 100.0F), vec3f_scale(metres.hi, 100.0F)};
    TEST_ASSERT_FLOAT_WITHIN(1.0E-4F, 13.0F / RENDER_LAB_FLY_CROSSING_S, render_lab_fly_start_speed(metres));
    TEST_ASSERT_FLOAT_WITHIN(1.0E-2F, 1300.0F / RENDER_LAB_FLY_CROSSING_S, render_lab_fly_start_speed(centimetres));
    TEST_ASSERT_EQUAL_FLOAT(FLY_MOTION_START_SPEED, render_lab_fly_start_speed(box3f_empty()));
}

#ifdef DEVICE_BUILD

#include <math.h>

#include "apps/render_lab/render_lab.h"
#include "apps/render_lab/tests/render_lab_test.h"
#include "math/scalar/units.h"
#include "testkit/cleanup.h"

#define FLY_TOLERANCE 1.0E-3F
/* Enough frames to pass the idle limit whatever the fixture's frame time. */
#define IDLE_FRAMES   ((RENDER_LAB_FLY_IDLE_MS / RENDER_LAB_TEST_FRAME_MS) + 2)
/* A drag sideways, as a steering line spells it; the camera may face up or
 * down, so a test asks only for at least half the yaw it asks for. */
#define DRAG_PIXELS   200
#define SPELL(x)      #x
#define SPELLED(x)    SPELL(x)
#define DRAG_LINE     "fly 0 0 0 " SPELLED(DRAG_PIXELS) " 0 0"
#define FORWARD_LINE  "fly 0 0 1 0 0 0"
/* Forward with the wheel one notch up. */
#define FASTER_LINE   "fly 0 0 1 0 0 1"
#define STILL_LINE    "fly 0 0 0 0 0 0"
/* The scene a switch goes to: one with a camera of its own. */
#define OTHER_SCENE   "capybara"
/* Frames for a playing clip to carry the camera off its pose. */
#define PLAY_FRAMES   8

static render_lab_test_t saved;

static void
release_fixture(void) {
    (void)app_render_lab.console->handle("fly release");
    render_lab_test_exit(&saved);
}

static bool
console(const char* line) {
    return app_render_lab.console->handle(line);
}

static void
frames(int count) {
    for (int i = 0; i < count; i++) {
        render_lab_test_frame(false, NULL);
    }
}

static scene_t*
drawn_scene(scene_entity_t* camera) {
    scene_t* scene;
    TEST_ASSERT_TRUE(scene_active_camera(&scene, camera));
    return scene;
}

static transformf_t
camera_now(void) {
    scene_entity_t camera;
    scene_t* scene = drawn_scene(&camera);
    return scene_entity_world(scene, camera);
}

static bool
clip_paused(void) {
    scene_entity_t camera;
    scene_t* scene = drawn_scene(&camera);
    return scene_animator_paused(scene, camera, SCENE_ANIMATOR_CURRENT);
}

/* How far one fixture frame flies at the start speed. */
static float
stride(void) {
    scene_entity_t camera;
    scene_t* scene = drawn_scene(&camera);
    return render_lab_fly_start_speed(scene_bounds(scene)) * (float)RENDER_LAB_TEST_FRAME_MS / (float)MATH_MS_PER_S;
}

static float
distance(vec3f_t a, vec3f_t b) {
    return vec3f_length(vec3f_sub(a, b));
}

/* Sponza, a frame drawn, and where its camera stands before any flight. */
static transformf_t
fixture(void) {
    render_lab_test_enter(&saved, "sponza");
    suite_set_test_cleanup(release_fixture);
    render_lab_test_frame(false, NULL);
    return camera_now();
}

/* The same, with the camera's clip held by "render pose", so nothing but a
 * flight moves it. */
static transformf_t
held_fixture(void) {
    (void)fixture();
    TEST_ASSERT_TRUE(console("pose 0"));
    TEST_ASSERT_TRUE(clip_paused());
    render_lab_test_frame(false, NULL);
    return camera_now();
}

static void
expect_at(vec3f_t expected, vec3f_t actual) {
    TEST_ASSERT_FLOAT_WITHIN(FLY_TOLERANCE, expected.x, actual.x);
    TEST_ASSERT_FLOAT_WITHIN(FLY_TOLERANCE, expected.y, actual.y);
    TEST_ASSERT_FLOAT_WITHIN(FLY_TOLERANCE, expected.z, actual.z);
}

static void
test_forward_flies_the_drawn_camera_and_release_puts_it_back(void) {
    const transformf_t home = fixture();
    TEST_ASSERT_TRUE(console(FORWARD_LINE));
    render_lab_test_frame(false, NULL);
    const vec3f_t ahead = quatf_rotate(home.rotation, vec3f_forward());
    expect_at(vec3f_add(home.position, vec3f_scale(ahead, stride())), camera_now().position);

    /* The camera's clip is paused: a frame with the keys let go leaves it still. */
    TEST_ASSERT_TRUE(console(STILL_LINE));
    const vec3f_t flown = camera_now().position;
    render_lab_test_frame(false, NULL);
    expect_at(flown, camera_now().position);

    TEST_ASSERT_TRUE(console("fly release"));
    expect_at(home.position, camera_now().position);
}

/* A line's keys stay held, frame after frame, until the next line. */
static void
test_held_keys_fly_every_frame_until_the_next_line(void) {
    const transformf_t home = held_fixture();
    TEST_ASSERT_TRUE(console(FORWARD_LINE));
    frames(2);
    TEST_ASSERT_FLOAT_WITHIN(FLY_TOLERANCE, 2.0F * stride(), distance(home.position, camera_now().position));
}

/* A drag turns once, on the frame after it; the frames after leave the turn. */
static void
test_a_drag_turns_the_drawn_camera_once(void) {
    const transformf_t home = held_fixture();
    const vec3f_t before = quatf_rotate(home.rotation, vec3f_forward());
    TEST_ASSERT_TRUE(console(DRAG_LINE));
    render_lab_test_frame(false, NULL);
    const quatf_t turned = camera_now().rotation;
    const vec3f_t after = quatf_rotate(turned, vec3f_forward());
    TEST_ASSERT_TRUE(vec3f_dot(before, after) < cosf(0.5F * DRAG_PIXELS * FLY_MOTION_RADIANS_PER_PIXEL));
    expect_at(home.position, camera_now().position);
    render_lab_test_frame(false, NULL);
    expect_at(after, quatf_rotate(camera_now().rotation, vec3f_forward()));
}

/* Two lines' drags before one frame turn as far as one line with both. */
static void
test_drags_between_frames_add_up(void) {
    const transformf_t home = held_fixture();
    const vec3f_t before = quatf_rotate(home.rotation, vec3f_forward());
    TEST_ASSERT_TRUE(console(DRAG_LINE));
    TEST_ASSERT_TRUE(console(DRAG_LINE));
    render_lab_test_frame(false, NULL);
    const vec3f_t after = quatf_rotate(camera_now().rotation, vec3f_forward());
    /* A level yaw of twice the drag, whatever the pitch: the heading turns that far. */
    const float yaw = 2.0F * DRAG_PIXELS * FLY_MOTION_RADIANS_PER_PIXEL;
    const vec3f_t level_before = vec3f_normalize((vec3f_t){before.x, 0.0F, before.z});
    const vec3f_t level_after = vec3f_normalize((vec3f_t){after.x, 0.0F, after.z});
    TEST_ASSERT_FLOAT_WITHIN(FLY_TOLERANCE, cosf(yaw), vec3f_dot(level_before, level_after));
}

/* The wheel scales the speed from the frame it arrives on. */
static void
test_the_wheel_speeds_the_flight(void) {
    const transformf_t home = held_fixture();
    TEST_ASSERT_TRUE(console(FASTER_LINE));
    render_lab_test_frame(false, NULL);
    TEST_ASSERT_FLOAT_WITHIN(FLY_TOLERANCE, FLY_MOTION_SPEED_STEP * stride(),
                             distance(home.position, camera_now().position));
}

/* With no line for the idle limit, the camera goes home and its clip stays
 * as "render pose" held it. */
static void
test_a_flight_with_no_lines_ends_where_it_began(void) {
    const transformf_t home = held_fixture();
    TEST_ASSERT_TRUE(console(FORWARD_LINE));
    render_lab_test_frame(false, NULL);
    TEST_ASSERT_TRUE(distance(home.position, camera_now().position) > 0.5F * stride());
    frames(IDLE_FRAMES);
    expect_at(home.position, camera_now().position);
    TEST_ASSERT_TRUE(clip_paused());
}

/* A line every frame keeps the flight going past the idle limit. */
static void
test_lines_keep_a_flight_past_the_idle_limit(void) {
    const transformf_t home = held_fixture();
    for (int i = 0; i < IDLE_FRAMES; i++) {
        TEST_ASSERT_TRUE(console(FORWARD_LINE));
        render_lab_test_frame(false, NULL);
    }
    TEST_ASSERT_FLOAT_WITHIN(FLY_TOLERANCE * IDLE_FRAMES, (float)IDLE_FRAMES * stride(),
                             distance(home.position, camera_now().position));
}

/* A clip that played before the flight plays again after it. */
static void
test_release_lets_a_playing_clip_move_the_camera_again(void) {
    const transformf_t home = fixture();
    TEST_ASSERT_FALSE(clip_paused());
    TEST_ASSERT_TRUE(console(FORWARD_LINE));
    render_lab_test_frame(false, NULL);
    TEST_ASSERT_TRUE(clip_paused());
    TEST_ASSERT_TRUE(console("fly release"));
    TEST_ASSERT_FALSE(clip_paused());
    frames(PLAY_FRAMES);
    TEST_ASSERT_TRUE(distance(home.position, camera_now().position) > FLY_TOLERANCE);
}

/* A scene switch ends the flight: the keys held fly nothing in the new scene. */
static void
test_a_scene_switch_ends_the_flight(void) {
    (void)held_fixture();
    TEST_ASSERT_TRUE(console(FORWARD_LINE));
    render_lab_test_frame(false, NULL);
    TEST_ASSERT_TRUE(console("scene " OTHER_SCENE));
    frames(2);
    TEST_ASSERT_EQUAL_STRING(OTHER_SCENE, render_lab_start_scene_key);
    const transformf_t there = camera_now();
    render_lab_test_frame(false, NULL);
    expect_at(there.position, camera_now().position);
}

#endif

void
run_render_lab_fly_suite(void) {
    RUN_TEST(test_a_steering_line_reads_its_six_numbers);
    RUN_TEST(test_a_malformed_steering_line_writes_nothing);
    RUN_TEST(test_a_flight_crosses_its_scene_in_the_same_time_whatever_its_units);
#ifdef DEVICE_BUILD
    RUN_TEST(test_forward_flies_the_drawn_camera_and_release_puts_it_back);
    RUN_TEST(test_held_keys_fly_every_frame_until_the_next_line);
    RUN_TEST(test_a_drag_turns_the_drawn_camera_once);
    RUN_TEST(test_drags_between_frames_add_up);
    RUN_TEST(test_the_wheel_speeds_the_flight);
    RUN_TEST(test_a_flight_with_no_lines_ends_where_it_began);
    RUN_TEST(test_lines_keep_a_flight_past_the_idle_limit);
    RUN_TEST(test_release_lets_a_playing_clip_move_the_camera_again);
    RUN_TEST(test_a_scene_switch_ends_the_flight);
#endif
}

SUITE_REGISTER(run_render_lab_fly_suite);

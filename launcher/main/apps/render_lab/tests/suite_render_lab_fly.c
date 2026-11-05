/*
 * render_lab_fly: a steering line reads whole or not at all (portable), and
 * on the device the lines fly the drawn camera and every way a flight ends
 * puts the camera back where it began.
 */
#include "suites.h"

#include "testkit/suite.h"
#include "unity.h"

#include "apps/render_lab/render_lab_fly.h"

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

#include "apps/render_lab/tests/render_lab_test.h"
#include "math/scalar/units.h"
#include "testkit/cleanup.h"

#define FLY_TOLERANCE 1.0E-3F
/* Enough frames to pass the idle limit whatever the fixture's frame time. */
#define IDLE_FRAMES   ((RENDER_LAB_FLY_IDLE_MS / RENDER_LAB_TEST_FRAME_MS) + 2)
/* A drag sideways, as a steering line spells it; the camera may face up or
 * down, so the test asks only for at least half the yaw it asks for. */
#define DRAG_PIXELS   200
#define SPELL(x)      #x
#define SPELLED(x)    SPELL(x)
#define DRAG_LINE     "fly 0 0 0 " SPELLED(DRAG_PIXELS) " 0 0"

static render_lab_test_t saved;

static void
release_fixture(void) {
    (void)app_render_lab.console->handle("fly release");
    render_lab_test_exit(&saved);
}

/* Sponza, a frame drawn, and where its camera stands before any flight. */
static transformf_t
fixture(void) {
    render_lab_test_enter(&saved, "sponza");
    suite_set_test_cleanup(release_fixture);
    render_lab_test_frame(false, NULL);
    scene_t* scene;
    scene_entity_t camera;
    TEST_ASSERT_TRUE(scene_active_camera(&scene, &camera));
    return scene_entity_local(scene, camera);
}

static transformf_t
camera_now(void) {
    scene_t* scene;
    scene_entity_t camera;
    TEST_ASSERT_TRUE(scene_active_camera(&scene, &camera));
    return scene_entity_local(scene, camera);
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
    TEST_ASSERT_TRUE(app_render_lab.console->handle("fly 0 0 1 0 0 0"));
    render_lab_test_frame(false, NULL);
    const vec3f_t ahead = quatf_rotate(home.rotation, vec3f_forward());
    scene_t* scene;
    scene_entity_t camera;
    TEST_ASSERT_TRUE(scene_active_camera(&scene, &camera));
    const float speed = render_lab_fly_start_speed(scene_bounds(scene));
    const float stride = speed * (float)RENDER_LAB_TEST_FRAME_MS / (float)MATH_MS_PER_S;
    expect_at(vec3f_add(home.position, vec3f_scale(ahead, stride)), camera_now().position);

    /* The camera's clip is paused: a frame with the keys let go leaves it still. */
    TEST_ASSERT_TRUE(app_render_lab.console->handle("fly 0 0 0 0 0 0"));
    const vec3f_t flown = camera_now().position;
    render_lab_test_frame(false, NULL);
    expect_at(flown, camera_now().position);

    TEST_ASSERT_TRUE(app_render_lab.console->handle("fly release"));
    expect_at(home.position, camera_now().position);
}

static void
test_a_drag_turns_the_drawn_camera(void) {
    const transformf_t home = fixture();
    const vec3f_t before = quatf_rotate(home.rotation, vec3f_forward());
    TEST_ASSERT_TRUE(app_render_lab.console->handle(DRAG_LINE));
    render_lab_test_frame(false, NULL);
    const vec3f_t after = quatf_rotate(camera_now().rotation, vec3f_forward());
    TEST_ASSERT_TRUE(vec3f_dot(before, after) < cosf(0.5F * DRAG_PIXELS * FLY_MOTION_RADIANS_PER_PIXEL));
    expect_at(home.position, camera_now().position);
}

static void
test_a_flight_with_no_lines_ends_where_it_began(void) {
    const transformf_t home = fixture();
    TEST_ASSERT_TRUE(app_render_lab.console->handle("fly 0 0 1 0 0 0"));
    for (int i = 0; i < IDLE_FRAMES; i++) {
        render_lab_test_frame(false, NULL);
    }
    expect_at(home.position, camera_now().position);
}

#endif

void
run_render_lab_fly_suite(void) {
    RUN_TEST(test_a_steering_line_reads_its_six_numbers);
    RUN_TEST(test_a_malformed_steering_line_writes_nothing);
    RUN_TEST(test_a_flight_crosses_its_scene_in_the_same_time_whatever_its_units);
#ifdef DEVICE_BUILD
    RUN_TEST(test_forward_flies_the_drawn_camera_and_release_puts_it_back);
    RUN_TEST(test_a_drag_turns_the_drawn_camera);
    RUN_TEST(test_a_flight_with_no_lines_ends_where_it_began);
#endif
}

SUITE_REGISTER(run_render_lab_fly_suite);

/*
 * orbit_motion_touch: a drag turns the orbit by the finger's travel the
 * way the picture is held; touches that land in the shell's strips (the home edge
 * and the one opposite) never steer or tap; two quick taps ask for home,
 * and anything else between them does not.
 */

#include <math.h>
#include <stdlib.h>

#include "gfx/gfx.h"
#include "input/orbit_motion_touch.h"
#include "suites.h"
#include "unity.h"

#define FRAME_MS     16
#define DRAG_PX      40
#define TURN_EPSILON 1.0E-5F
#define HOME_EDGE    GESTURE_EDGE_BOTTOM
#define CENTRE_X     (GFX_WIDTH / 2)
#define CENTRE_Y     (GFX_HEIGHT / 2)
#define INSIDE_STRIP (GESTURE_HOME_ZONE_DEPTH / 2)

static const viewport_t PORTRAIT = {GFX_WIDTH, GFX_HEIGHT, 0};

static float
per_px(void) {
    return ORBIT_MOTION_TOUCH_TURN_PER_SHORT_SIDE / (float)(GFX_WIDTH < GFX_HEIGHT ? GFX_WIDTH : GFX_HEIGHT);
}

static input_t
finger(bool pressed, bool down, bool released, int press_x, int press_y, int x, int y) {
    input_t in = {0};
    in.pressed = pressed;
    in.down = down;
    in.released = released;
    in.press_x = press_x;
    in.press_y = press_y;
    in.x = x;
    in.y = y;
    return in;
}

/* Lands at (x0, y0), moves to (x1, y1) over one frame and holds there; the
 * steering of the moving frame. */
static orbit_motion_input_t
drag_on(orbit_motion_touch_t* t, viewport_t viewport, int x0, int y0, int x1, int y1) {
    const input_t down = finger(true, true, false, x0, y0, x0, y0);
    (void)orbit_motion_touch_step(t, &down, FRAME_MS, viewport, HOME_EDGE);
    const input_t moved = finger(false, true, false, x0, y0, x1, y1);
    return orbit_motion_touch_step(t, &moved, FRAME_MS, viewport, HOME_EDGE);
}

static orbit_motion_input_t
lift_at(orbit_motion_touch_t* t, int press_x, int press_y, int x, int y) {
    const input_t up = finger(false, false, true, press_x, press_y, x, y);
    return orbit_motion_touch_step(t, &up, FRAME_MS, PORTRAIT, HOME_EDGE);
}

/* A tap at the centre; its lift's steering. */
static orbit_motion_input_t
tap(orbit_motion_touch_t* t) {
    const input_t down = finger(true, true, false, CENTRE_X, CENTRE_Y, CENTRE_X, CENTRE_Y);
    (void)orbit_motion_touch_step(t, &down, FRAME_MS, PORTRAIT, HOME_EDGE);
    return lift_at(t, CENTRE_X, CENTRE_Y, CENTRE_X, CENTRE_Y);
}

static void
wait_ms(orbit_motion_touch_t* t, uint32_t ms) {
    const input_t idle = {0};
    (void)orbit_motion_touch_step(t, &idle, ms, PORTRAIT, HOME_EDGE);
}

void
test_orbit_motion_touch_a_drag_turns_by_its_travel_right_turning_the_eye_left_and_down_raising_it(void) {
    orbit_motion_touch_t t;
    orbit_motion_touch_init(&t);
    const orbit_motion_input_t out = drag_on(&t, PORTRAIT, CENTRE_X, CENTRE_Y, CENTRE_X + DRAG_PX, CENTRE_Y + DRAG_PX);
    TEST_ASSERT_TRUE(out.held);
    TEST_ASSERT_FLOAT_WITHIN(TURN_EPSILON, -DRAG_PX * per_px(), out.yaw_turn);
    TEST_ASSERT_FLOAT_WITHIN(TURN_EPSILON, DRAG_PX * per_px(), out.pitch_turn);
}

void
test_orbit_motion_touch_a_drag_across_the_shorter_side_turns_the_named_amount(void) {
    orbit_motion_touch_t t;
    orbit_motion_touch_init(&t);
    const int shorter = GFX_WIDTH < GFX_HEIGHT ? GFX_WIDTH : GFX_HEIGHT;
    const orbit_motion_input_t out =
        drag_on(&t, PORTRAIT, CENTRE_X - shorter / 2, CENTRE_Y, CENTRE_X + shorter / 2, CENTRE_Y);
    TEST_ASSERT_FLOAT_WITHIN(TURN_EPSILON, -ORBIT_MOTION_TOUCH_TURN_PER_SHORT_SIDE, out.yaw_turn);
}

void
test_orbit_motion_touch_a_turned_panel_steers_by_the_upright_picture(void) {
    /* A quarter turn makes the panel's x the picture's y: a drag along the
     * panel's x tilts rather than turns. */
    orbit_motion_touch_t t;
    orbit_motion_touch_init(&t);
    const viewport_t turned = {GFX_WIDTH, GFX_HEIGHT, 1};
    const orbit_motion_input_t out = drag_on(&t, turned, CENTRE_X, CENTRE_Y, CENTRE_X + DRAG_PX, CENTRE_Y);
    TEST_ASSERT_FLOAT_WITHIN(TURN_EPSILON, 0.0F, out.yaw_turn);
    TEST_ASSERT_FLOAT_WITHIN(TURN_EPSILON, DRAG_PX * per_px(), fabsf(out.pitch_turn));
}

void
test_orbit_motion_touch_a_drag_from_the_home_strip_or_the_one_opposite_never_steers(void) {
    const int starts[][2] = {{CENTRE_X, GFX_HEIGHT - INSIDE_STRIP}, {CENTRE_X, INSIDE_STRIP}};
    for (unsigned i = 0; i < sizeof starts / sizeof starts[0]; i++) {
        orbit_motion_touch_t t;
        orbit_motion_touch_init(&t);
        const orbit_motion_input_t out =
            drag_on(&t, PORTRAIT, starts[i][0], starts[i][1], starts[i][0] + DRAG_PX, CENTRE_Y);
        TEST_ASSERT_FALSE(out.held);
        TEST_ASSERT_EQUAL_FLOAT(0.0F, out.yaw_turn);
        TEST_ASSERT_EQUAL_FLOAT(0.0F, out.pitch_turn);
    }
}

void
test_orbit_motion_touch_a_drag_from_a_side_edge_steers_when_home_is_at_the_bottom(void) {
    orbit_motion_touch_t t;
    orbit_motion_touch_init(&t);
    const orbit_motion_input_t out = drag_on(&t, PORTRAIT, INSIDE_STRIP, CENTRE_Y, INSIDE_STRIP + DRAG_PX, CENTRE_Y);
    TEST_ASSERT_TRUE(out.held);
}

void
test_orbit_motion_touch_two_quick_taps_ask_for_home_once(void) {
    orbit_motion_touch_t t;
    orbit_motion_touch_init(&t);
    TEST_ASSERT_FALSE(tap(&t).reset);
    wait_ms(&t, ORBIT_MOTION_TOUCH_DOUBLE_TAP_MS / 2);
    TEST_ASSERT_TRUE(tap(&t).reset);
    wait_ms(&t, ORBIT_MOTION_TOUCH_DOUBLE_TAP_MS / 2);
    TEST_ASSERT_FALSE_MESSAGE(tap(&t).reset, "a third tap starts a new pair");
}

void
test_orbit_motion_touch_taps_too_far_apart_are_not_a_double_tap(void) {
    orbit_motion_touch_t t;
    orbit_motion_touch_init(&t);
    (void)tap(&t);
    wait_ms(&t, ORBIT_MOTION_TOUCH_DOUBLE_TAP_MS + FRAME_MS);
    TEST_ASSERT_FALSE(tap(&t).reset);
}

void
test_orbit_motion_touch_a_drag_between_two_taps_breaks_the_pair(void) {
    orbit_motion_touch_t t;
    orbit_motion_touch_init(&t);
    (void)tap(&t);
    (void)drag_on(&t, PORTRAIT, CENTRE_X, CENTRE_Y, CENTRE_X + DRAG_PX, CENTRE_Y);
    (void)lift_at(&t, CENTRE_X, CENTRE_Y, CENTRE_X + DRAG_PX, CENTRE_Y);
    TEST_ASSERT_FALSE(tap(&t).reset);
}

void
test_orbit_motion_touch_a_long_press_is_not_a_tap(void) {
    orbit_motion_touch_t t;
    orbit_motion_touch_init(&t);
    (void)tap(&t);
    const input_t down = finger(true, true, false, CENTRE_X, CENTRE_Y, CENTRE_X, CENTRE_Y);
    (void)orbit_motion_touch_step(&t, &down, FRAME_MS, PORTRAIT, HOME_EDGE);
    const input_t held = finger(false, true, false, CENTRE_X, CENTRE_Y, CENTRE_X, CENTRE_Y);
    (void)orbit_motion_touch_step(&t, &held, ORBIT_MOTION_TOUCH_TAP_MAX_MS, PORTRAIT, HOME_EDGE);
    TEST_ASSERT_FALSE(lift_at(&t, CENTRE_X, CENTRE_Y, CENTRE_X, CENTRE_Y).reset);
}

void
test_orbit_motion_touch_a_tap_within_its_slop_still_counts(void) {
    orbit_motion_touch_t t;
    orbit_motion_touch_init(&t);
    (void)tap(&t);
    (void)drag_on(&t, PORTRAIT, CENTRE_X, CENTRE_Y, CENTRE_X + ORBIT_MOTION_TOUCH_TAP_SLOP_PX, CENTRE_Y);
    TEST_ASSERT_TRUE(lift_at(&t, CENTRE_X, CENTRE_Y, CENTRE_X + ORBIT_MOTION_TOUCH_TAP_SLOP_PX, CENTRE_Y).reset);
}

void
test_orbit_motion_touch_taps_in_the_home_strip_never_ask_for_home(void) {
    orbit_motion_touch_t t;
    orbit_motion_touch_init(&t);
    const int y = GFX_HEIGHT - INSIDE_STRIP;
    for (int i = 0; i < 2; i++) {
        const input_t down = finger(true, true, false, CENTRE_X, y, CENTRE_X, y);
        (void)orbit_motion_touch_step(&t, &down, FRAME_MS, PORTRAIT, HOME_EDGE);
        TEST_ASSERT_FALSE(lift_at(&t, CENTRE_X, y, CENTRE_X, y).reset);
    }
}

static float
zoom_per_px(void) {
    return ORBIT_MOTION_TOUCH_ZOOM_PER_SHORT_SIDE / (float)(GFX_WIDTH < GFX_HEIGHT ? GFX_WIDTH : GFX_HEIGHT);
}

void
test_orbit_motion_touch_a_tap_then_a_drag_down_zooms_in_without_turning(void) {
    orbit_motion_touch_t t;
    orbit_motion_touch_init(&t);
    (void)tap(&t);
    const orbit_motion_input_t out = drag_on(&t, PORTRAIT, CENTRE_X, CENTRE_Y, CENTRE_X + DRAG_PX, CENTRE_Y + DRAG_PX);
    TEST_ASSERT_TRUE(out.held);
    TEST_ASSERT_FLOAT_WITHIN(TURN_EPSILON, -DRAG_PX * zoom_per_px(), out.zoom_turn);
    TEST_ASSERT_EQUAL_FLOAT(0.0F, out.yaw_turn);
    TEST_ASSERT_EQUAL_FLOAT(0.0F, out.pitch_turn);
}

void
test_orbit_motion_touch_a_tap_then_a_drag_up_zooms_out_and_never_resets(void) {
    orbit_motion_touch_t t;
    orbit_motion_touch_init(&t);
    (void)tap(&t);
    const orbit_motion_input_t out = drag_on(&t, PORTRAIT, CENTRE_X, CENTRE_Y, CENTRE_X, CENTRE_Y - DRAG_PX);
    TEST_ASSERT_FLOAT_WITHIN(TURN_EPSILON, DRAG_PX * zoom_per_px(), out.zoom_turn);
    TEST_ASSERT_FALSE(lift_at(&t, CENTRE_X, CENTRE_Y, CENTRE_X, CENTRE_Y - DRAG_PX).reset);
}

void
test_orbit_motion_touch_a_lone_drag_turns_and_never_zooms(void) {
    orbit_motion_touch_t t;
    orbit_motion_touch_init(&t);
    const orbit_motion_input_t out = drag_on(&t, PORTRAIT, CENTRE_X, CENTRE_Y, CENTRE_X, CENTRE_Y + DRAG_PX);
    TEST_ASSERT_EQUAL_FLOAT(0.0F, out.zoom_turn);
    TEST_ASSERT_FLOAT_WITHIN(TURN_EPSILON, DRAG_PX * per_px(), out.pitch_turn);
}

void
test_orbit_motion_touch_a_second_press_in_the_home_strip_neither_zooms_nor_turns(void) {
    orbit_motion_touch_t t;
    orbit_motion_touch_init(&t);
    (void)tap(&t);
    const int y = GFX_HEIGHT - INSIDE_STRIP;
    const orbit_motion_input_t out = drag_on(&t, PORTRAIT, CENTRE_X, y, CENTRE_X, y - DRAG_PX);
    TEST_ASSERT_FALSE(out.held);
    TEST_ASSERT_EQUAL_FLOAT(0.0F, out.zoom_turn);
}

void
run_orbit_motion_touch_suite(void) {
    RUN_TEST(test_orbit_motion_touch_a_tap_then_a_drag_down_zooms_in_without_turning);
    RUN_TEST(test_orbit_motion_touch_a_tap_then_a_drag_up_zooms_out_and_never_resets);
    RUN_TEST(test_orbit_motion_touch_a_lone_drag_turns_and_never_zooms);
    RUN_TEST(test_orbit_motion_touch_a_second_press_in_the_home_strip_neither_zooms_nor_turns);
    RUN_TEST(test_orbit_motion_touch_a_drag_turns_by_its_travel_right_turning_the_eye_left_and_down_raising_it);
    RUN_TEST(test_orbit_motion_touch_a_drag_across_the_shorter_side_turns_the_named_amount);
    RUN_TEST(test_orbit_motion_touch_a_turned_panel_steers_by_the_upright_picture);
    RUN_TEST(test_orbit_motion_touch_a_drag_from_the_home_strip_or_the_one_opposite_never_steers);
    RUN_TEST(test_orbit_motion_touch_a_drag_from_a_side_edge_steers_when_home_is_at_the_bottom);
    RUN_TEST(test_orbit_motion_touch_two_quick_taps_ask_for_home_once);
    RUN_TEST(test_orbit_motion_touch_taps_too_far_apart_are_not_a_double_tap);
    RUN_TEST(test_orbit_motion_touch_a_drag_between_two_taps_breaks_the_pair);
    RUN_TEST(test_orbit_motion_touch_a_long_press_is_not_a_tap);
    RUN_TEST(test_orbit_motion_touch_a_tap_within_its_slop_still_counts);
    RUN_TEST(test_orbit_motion_touch_taps_in_the_home_strip_never_ask_for_home);
}

SUITE_REGISTER(run_orbit_motion_touch_suite);

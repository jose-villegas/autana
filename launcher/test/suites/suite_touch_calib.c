/*
 * Portable suite: touch_calib, a fitted misreport undone, point by point.
 */

#include "suites.h"
#include "unity.h"

#include "input/touch/touch_calib.h"
#include "input/touch/touch_fsm.h"

/* A panel that stretches both axes and shifts them, the shape measured. */
static const touch_calib_fit_t STRETCH = {.xx = 1.12f, .xy = 0.0f, .x0 = -23.0f, .yx = 0.0f, .yy = 1.18f, .y0 = -29.0f};

static void
reported(touch_calib_fit_t fit, int x, int y, vec2i_t* point) {
    point->x = (int)(fit.xx * x + fit.xy * y + fit.x0 + 0.5f);
    point->y = (int)(fit.yx * x + fit.yy * y + fit.y0 + 0.5f);
}

static void
assert_recovers(touch_calib_fit_t fit, int x, int y) {
    const touch_calib_t calib = touch_calib_from_fit(fit);
    vec2i_t point;
    reported(fit, x, y, &point);
    point = touch_calib_apply(&calib, 368, 448, point);
    TEST_ASSERT_INT_WITHIN(1, x, point.x);
    TEST_ASSERT_INT_WITHIN(1, y, point.y);
}

static void
test_a_stretched_tap_is_brought_back_to_where_it_was_aimed(void) {
    assert_recovers(STRETCH, 184, 224);
    assert_recovers(STRETCH, 60, 80);
    assert_recovers(STRETCH, 300, 380);
}

/* The cross terms matter: a sheared panel moves x with y. */
static void
test_a_sheared_panel_is_undone_too(void) {
    const touch_calib_fit_t shear = {.xx = 1.1f, .xy = 0.05f, .x0 = -20.0f, .yx = 0.03f, .yy = 1.2f, .y0 = -30.0f};
    assert_recovers(shear, 100, 300);
    assert_recovers(shear, 250, 60);
}

static void
test_an_honest_panel_changes_nothing(void) {
    const touch_calib_fit_t identity = {.xx = 1.0f, .yy = 1.0f};
    const touch_calib_t calib = touch_calib_from_fit(identity);
    vec2i_t point = {123, 321};
    point = touch_calib_apply(&calib, 368, 448, point);
    TEST_ASSERT_EQUAL_INT(123, point.x);
    TEST_ASSERT_EQUAL_INT(321, point.y);
}

/* A report pinned at the panel's edge corrects to a point inside it, never
 * past it: the correction of an edge report can land outside otherwise. */
static void
test_a_correction_stays_on_the_panel(void) {
    const touch_calib_fit_t shrink = {.xx = 0.9f, .yy = 0.9f};
    const touch_calib_t calib = touch_calib_from_fit(shrink);
    vec2i_t point = {367, 447};
    point = touch_calib_apply(&calib, 368, 448, point);
    TEST_ASSERT_EQUAL_INT(367, point.x);
    TEST_ASSERT_EQUAL_INT(447, point.y);
    point = (vec2i_t){0};
    const touch_calib_t shift =
        touch_calib_from_fit((touch_calib_fit_t){.xx = 1.0f, .x0 = 20.0f, .yy = 1.0f, .y0 = 20.0f});
    point = touch_calib_apply(&shift, 368, 448, point);
    TEST_ASSERT_EQUAL_INT(0, point.x);
    TEST_ASSERT_EQUAL_INT(0, point.y);
}

static void
test_a_controller_point_is_corrected_once_before_the_fsm(void) {
    const touch_calib_t calib = touch_calib_from_fit(STRETCH);
    vec2i_t point;
    reported(STRETCH, 100, 300, &point);

    touch_fsm_t fsm;
    input_t in;
    touch_fsm_init(&fsm);
    touch_calib_prepare_point(false, true, &point, true, &calib, 368, 448);
    touch_fsm_update(&fsm, true, point, 0);
    touch_fsm_take(&fsm, &in);
    TEST_ASSERT_INT_WITHIN(1, 100, in.point.x);
    TEST_ASSERT_INT_WITHIN(1, 300, in.point.y);
}

static void
test_a_controller_point_is_unchanged_when_calibration_is_off(void) {
    const touch_calib_t calib = touch_calib_from_fit(STRETCH);
    vec2i_t point;
    reported(STRETCH, 100, 300, &point);
    const int reported_x = point.x, reported_y = point.y;

    touch_fsm_t fsm;
    input_t in;
    touch_fsm_init(&fsm);
    touch_calib_prepare_point(false, true, &point, false, &calib, 368, 448);
    touch_fsm_update(&fsm, true, point, 0);
    touch_fsm_take(&fsm, &in);
    TEST_ASSERT_EQUAL_INT(reported_x, in.point.x);
    TEST_ASSERT_EQUAL_INT(reported_y, in.point.y);
}

static void
test_an_injected_point_is_not_corrected_before_the_fsm(void) {
    const touch_calib_t calib = touch_calib_from_fit(STRETCH);
    vec2i_t point = {100, 300};

    touch_fsm_t fsm;
    input_t in;
    touch_fsm_init(&fsm);
    touch_calib_prepare_point(true, true, &point, true, &calib, 368, 448);
    touch_fsm_update(&fsm, true, point, 0);
    touch_fsm_take(&fsm, &in);
    TEST_ASSERT_EQUAL_INT(100, in.point.x);
    TEST_ASSERT_EQUAL_INT(300, in.point.y);
}

static void
test_an_injected_lift_reaches_the_fsm_without_touching_the_point(void) {
    const touch_calib_t calib = touch_calib_from_fit(STRETCH);
    touch_fsm_t fsm;
    input_t in;
    vec2i_t point = {100, 300};
    touch_fsm_init(&fsm);

    touch_calib_prepare_point(true, true, &point, true, &calib, 368, 448);
    touch_fsm_update(&fsm, true, point, 0);
    touch_fsm_take(&fsm, &in);
    point.x = 71;
    point.y = 83;
    touch_calib_prepare_point(true, false, &point, true, &calib, 368, 448);
    touch_fsm_update(&fsm, false, point, TOUCH_RELEASE_QUIET_US + 1);
    touch_fsm_take(&fsm, &in);

    TEST_ASSERT_TRUE(in.released);
    TEST_ASSERT_FALSE(in.down);
    TEST_ASSERT_EQUAL_INT(71, point.x);
    TEST_ASSERT_EQUAL_INT(83, point.y);
}

static void
test_controller_press_and_move_are_corrected_once_each_before_release(void) {
    const touch_calib_t calib = touch_calib_from_fit(STRETCH);
    touch_fsm_t fsm;
    input_t in;
    vec2i_t point;
    touch_fsm_init(&fsm);

    reported(STRETCH, 100, 300, &point);
    touch_calib_prepare_point(false, true, &point, true, &calib, 368, 448);
    touch_fsm_update(&fsm, true, point, 0);
    touch_fsm_take(&fsm, &in);
    TEST_ASSERT_TRUE(in.pressed);
    TEST_ASSERT_INT_WITHIN(1, 100, in.press.x);
    TEST_ASSERT_INT_WITHIN(1, 300, in.press.y);

    reported(STRETCH, 250, 60, &point);
    touch_calib_prepare_point(false, true, &point, true, &calib, 368, 448);
    touch_fsm_update(&fsm, true, point, 1000);
    touch_fsm_take(&fsm, &in);
    TEST_ASSERT_FALSE(in.pressed);
    TEST_ASSERT_INT_WITHIN(1, 250, in.point.x);
    TEST_ASSERT_INT_WITHIN(1, 60, in.point.y);

    point.x = 71;
    point.y = 83;
    touch_calib_prepare_point(false, false, &point, true, &calib, 368, 448);
    touch_fsm_update(&fsm, false, point, TOUCH_RELEASE_QUIET_US + 1001);
    touch_fsm_take(&fsm, &in);
    TEST_ASSERT_TRUE(in.released);
    TEST_ASSERT_INT_WITHIN(1, 100, in.press.x);
    TEST_ASSERT_INT_WITHIN(1, 300, in.press.y);
    TEST_ASSERT_INT_WITHIN(1, 250, in.point.x);
    TEST_ASSERT_INT_WITHIN(1, 60, in.point.y);
    TEST_ASSERT_EQUAL_INT(71, point.x);
    TEST_ASSERT_EQUAL_INT(83, point.y);
}

void
run_touch_calib_suite(void) {
    RUN_TEST(test_a_stretched_tap_is_brought_back_to_where_it_was_aimed);
    RUN_TEST(test_a_sheared_panel_is_undone_too);
    RUN_TEST(test_an_honest_panel_changes_nothing);
    RUN_TEST(test_a_correction_stays_on_the_panel);
    RUN_TEST(test_a_controller_point_is_corrected_once_before_the_fsm);
    RUN_TEST(test_a_controller_point_is_unchanged_when_calibration_is_off);
    RUN_TEST(test_an_injected_point_is_not_corrected_before_the_fsm);
    RUN_TEST(test_an_injected_lift_reaches_the_fsm_without_touching_the_point);
    RUN_TEST(test_controller_press_and_move_are_corrected_once_each_before_release);
}

SUITE_REGISTER(run_touch_calib_suite);

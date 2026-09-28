/*
 * Portable suite: touch_calib - a fitted misreport undone, point by point.
 */

#include "suites.h"
#include "unity.h"

#include "input/touch_calib.h"
#include "input/touch_fsm.h"
#include "input/touch_point.h"

/* A panel that stretches both axes and shifts them, the shape measured. */
static const touch_calib_fit_t STRETCH = {.xx = 1.12f, .xy = 0.0f, .x0 = -23.0f, .yx = 0.0f, .yy = 1.18f, .y0 = -29.0f};

static void
reported(touch_calib_fit_t fit, int x, int y, int* rx, int* ry) {
    *rx = (int)(fit.xx * x + fit.xy * y + fit.x0 + 0.5f);
    *ry = (int)(fit.yx * x + fit.yy * y + fit.y0 + 0.5f);
}

static void
assert_recovers(touch_calib_fit_t fit, int x, int y) {
    const touch_calib_t calib = touch_calib_from_fit(fit);
    int cx, cy;
    reported(fit, x, y, &cx, &cy);
    touch_calib_apply(&calib, 368, 448, &cx, &cy);
    TEST_ASSERT_INT_WITHIN(1, x, cx);
    TEST_ASSERT_INT_WITHIN(1, y, cy);
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
    int x = 123, y = 321;
    touch_calib_apply(&calib, 368, 448, &x, &y);
    TEST_ASSERT_EQUAL_INT(123, x);
    TEST_ASSERT_EQUAL_INT(321, y);
}

/* A report pinned at the panel's edge corrects to a point inside it, never
 * past it: the correction of an edge report can land outside otherwise. */
static void
test_a_correction_stays_on_the_panel(void) {
    const touch_calib_fit_t shrink = {.xx = 0.9f, .yy = 0.9f};
    const touch_calib_t calib = touch_calib_from_fit(shrink);
    int x = 367, y = 447;
    touch_calib_apply(&calib, 368, 448, &x, &y);
    TEST_ASSERT_EQUAL_INT(367, x);
    TEST_ASSERT_EQUAL_INT(447, y);
    x = 0;
    y = 0;
    const touch_calib_t shift =
        touch_calib_from_fit((touch_calib_fit_t){.xx = 1.0f, .x0 = 20.0f, .yy = 1.0f, .y0 = 20.0f});
    touch_calib_apply(&shift, 368, 448, &x, &y);
    TEST_ASSERT_EQUAL_INT(0, x);
    TEST_ASSERT_EQUAL_INT(0, y);
}

static void
test_a_controller_point_is_corrected_once_before_the_fsm(void) {
    const touch_calib_t calib = touch_calib_from_fit(STRETCH);
    int x, y;
    reported(STRETCH, 100, 300, &x, &y);

    touch_fsm_t fsm;
    input_t in;
    touch_fsm_init(&fsm);
    touch_point_update_fsm(&fsm, false, true, &x, &y, 0, true, &calib, 368, 448);
    touch_fsm_take(&fsm, &in);
    TEST_ASSERT_INT_WITHIN(1, 100, in.x);
    TEST_ASSERT_INT_WITHIN(1, 300, in.y);
}

static void
test_a_controller_point_is_unchanged_when_calibration_is_off(void) {
    const touch_calib_t calib = touch_calib_from_fit(STRETCH);
    int x, y;
    reported(STRETCH, 100, 300, &x, &y);
    const int reported_x = x, reported_y = y;

    touch_fsm_t fsm;
    input_t in;
    touch_fsm_init(&fsm);
    touch_point_update_fsm(&fsm, false, true, &x, &y, 0, false, &calib, 368, 448);
    touch_fsm_take(&fsm, &in);
    TEST_ASSERT_EQUAL_INT(reported_x, in.x);
    TEST_ASSERT_EQUAL_INT(reported_y, in.y);
}

static void
test_an_injected_point_is_not_corrected_before_the_fsm(void) {
    const touch_calib_t calib = touch_calib_from_fit(STRETCH);
    int x = 100, y = 300;

    touch_fsm_t fsm;
    input_t in;
    touch_fsm_init(&fsm);
    touch_point_update_fsm(&fsm, true, true, &x, &y, 0, true, &calib, 368, 448);
    touch_fsm_take(&fsm, &in);
    TEST_ASSERT_EQUAL_INT(100, in.x);
    TEST_ASSERT_EQUAL_INT(300, in.y);
}

static void
test_an_injected_lift_reaches_the_fsm_without_touching_the_point(void) {
    const touch_calib_t calib = touch_calib_from_fit(STRETCH);
    touch_fsm_t fsm;
    input_t in;
    int x = 100, y = 300;
    touch_fsm_init(&fsm);

    touch_point_update_fsm(&fsm, true, true, &x, &y, 0, true, &calib, 368, 448);
    touch_fsm_take(&fsm, &in);
    x = 71;
    y = 83;
    touch_point_update_fsm(&fsm, true, false, &x, &y, TOUCH_RELEASE_QUIET_US + 1, true, &calib, 368, 448);
    touch_fsm_take(&fsm, &in);

    TEST_ASSERT_TRUE(in.released);
    TEST_ASSERT_FALSE(in.down);
    TEST_ASSERT_EQUAL_INT(71, x);
    TEST_ASSERT_EQUAL_INT(83, y);
}

static void
test_controller_press_and_move_are_corrected_once_each_before_release(void) {
    const touch_calib_t calib = touch_calib_from_fit(STRETCH);
    touch_fsm_t fsm;
    input_t in;
    int x, y;
    touch_fsm_init(&fsm);

    reported(STRETCH, 100, 300, &x, &y);
    touch_point_update_fsm(&fsm, false, true, &x, &y, 0, true, &calib, 368, 448);
    touch_fsm_take(&fsm, &in);
    TEST_ASSERT_TRUE(in.pressed);
    TEST_ASSERT_INT_WITHIN(1, 100, in.press_x);
    TEST_ASSERT_INT_WITHIN(1, 300, in.press_y);

    reported(STRETCH, 250, 60, &x, &y);
    touch_point_update_fsm(&fsm, false, true, &x, &y, 1000, true, &calib, 368, 448);
    touch_fsm_take(&fsm, &in);
    TEST_ASSERT_FALSE(in.pressed);
    TEST_ASSERT_INT_WITHIN(1, 250, in.x);
    TEST_ASSERT_INT_WITHIN(1, 60, in.y);

    x = 71;
    y = 83;
    touch_point_update_fsm(&fsm, false, false, &x, &y, TOUCH_RELEASE_QUIET_US + 1001, true, &calib, 368, 448);
    touch_fsm_take(&fsm, &in);
    TEST_ASSERT_TRUE(in.released);
    TEST_ASSERT_INT_WITHIN(1, 100, in.press_x);
    TEST_ASSERT_INT_WITHIN(1, 300, in.press_y);
    TEST_ASSERT_INT_WITHIN(1, 250, in.x);
    TEST_ASSERT_INT_WITHIN(1, 60, in.y);
    TEST_ASSERT_EQUAL_INT(71, x);
    TEST_ASSERT_EQUAL_INT(83, y);
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

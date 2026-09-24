/*
 * Portable suite: ui_canvas_marks - a window that stops being drawn has the
 * rect it was last painted over reported once, so ui_end() can repaint it.
 */

#include <string.h>

#include "suites.h"
#include "unity.h"

#include "ui/ui_canvas_marks.h"

static ui_canvas_marks_t marks;
static bool present[MU_CONTAINERPOOL_SIZE];

static void
fixture(void) {
    ui_canvas_marks_reset(&marks);
    memset(present, 0, sizeof present);
}

static int
take(mu_Rect* out) {
    return ui_canvas_marks_take_vanished(&marks, present, out, MU_CONTAINERPOOL_SIZE);
}

static void
test_a_window_still_drawn_is_not_reported(void) {
    fixture();
    ui_canvas_marks_painted(&marks, 3, mu_rect(0, 0, 368, 448));
    present[3] = true;
    mu_Rect out[MU_CONTAINERPOOL_SIZE];
    TEST_ASSERT_EQUAL_INT(0, take(out));
}

static void
test_a_window_that_stops_being_drawn_reports_its_last_rect_once(void) {
    fixture();
    ui_canvas_marks_painted(&marks, 3, mu_rect(0, 0, 368, 448));
    ui_canvas_marks_painted(&marks, 7, mu_rect(40, 100, 288, 220));
    present[3] = true;

    mu_Rect out[MU_CONTAINERPOOL_SIZE];
    TEST_ASSERT_EQUAL_INT(1, take(out));
    TEST_ASSERT_EQUAL_INT(100, out[0].y);
    TEST_ASSERT_EQUAL_INT(220, out[0].h);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, take(out), "a vanished window is reported once, not every frame after");
}

static void
test_the_rect_reported_is_where_it_was_last_painted(void) {
    fixture();
    ui_canvas_marks_painted(&marks, 5, mu_rect(10, 10, 50, 50));
    ui_canvas_marks_painted(&marks, 5, mu_rect(20, 30, 60, 70));
    mu_Rect out[MU_CONTAINERPOOL_SIZE];
    TEST_ASSERT_EQUAL_INT(1, take(out));
    TEST_ASSERT_EQUAL_INT(20, out[0].x);
    TEST_ASSERT_EQUAL_INT(30, out[0].y);
}

static void
test_a_slot_outside_the_pool_is_ignored(void) {
    fixture();
    ui_canvas_marks_painted(&marks, -1, mu_rect(0, 0, 10, 10));
    ui_canvas_marks_painted(&marks, MU_CONTAINERPOOL_SIZE, mu_rect(0, 0, 10, 10));
    mu_Rect out[MU_CONTAINERPOOL_SIZE];
    TEST_ASSERT_EQUAL_INT(0, take(out));
}

void
run_ui_canvas_marks_suite(void) {
    RUN_TEST(test_a_window_still_drawn_is_not_reported);
    RUN_TEST(test_a_window_that_stops_being_drawn_reports_its_last_rect_once);
    RUN_TEST(test_the_rect_reported_is_where_it_was_last_painted);
    RUN_TEST(test_a_slot_outside_the_pool_is_ignored);
}

SUITE_REGISTER(run_ui_canvas_marks_suite);

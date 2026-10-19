/* Portable geometry tests pass canvas width explicitly, so ui_centered_rect()
 * in ui.h needs neither a framebuffer nor a live microui context. */

#include "suites.h"
#include "unity.h"

#include "ui/ui.h"

/* Equal left/right margins, within a pixel: integer division means an odd
 * leftover pixel can fall on either side, never both. */
static void
assert_centered(int canvas_w, int w, const char* msg) {
    const recti_t r = ui_centered_rect(canvas_w, w, 1, 0);
    const int left = r.x;
    const int right = canvas_w - (r.x + r.w);

    TEST_ASSERT_TRUE_MESSAGE(left - right >= -1 && left - right <= 1, msg);
}

static void
test_centered_across_a_spread_of_canvas_and_width_pairs(void) {
    assert_centered(368, 300, "368/300 (the sand boot menu's own pair)");
    assert_centered(448, 300, "448/300 (the same button, wider canvas)");
    assert_centered(368, 240, "368/240 (the launcher's own pair)");
    assert_centered(448, 240, "448/240 (the same button, wider canvas)");
    assert_centered(200, 199, "odd canvas, odd-off-by-one width");
    assert_centered(201, 200, "odd canvas, even width");
    assert_centered(200, 200, "even canvas, even width");
    assert_centered(201, 201, "odd canvas, odd width");
}

static void
test_zero_margin_when_width_equals_canvas(void) {
    const recti_t r = ui_centered_rect(368, 368, 64, 10);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, r.x, "w == canvas_w must leave no margin on either side");
    TEST_ASSERT_EQUAL_INT(368, r.w);
}

static void
test_y_and_h_pass_through_unchanged(void) {
    const recti_t r1 = ui_centered_rect(368, 240, 64, 0);
    TEST_ASSERT_EQUAL_INT(0, r1.y);
    TEST_ASSERT_EQUAL_INT(64, r1.h);

    const recti_t r2 = ui_centered_rect(448, 300, 128, 137);
    TEST_ASSERT_EQUAL_INT_MESSAGE(137, r2.y, "y is a pass-through, not derived from canvas_w/w/h");
    TEST_ASSERT_EQUAL_INT(128, r2.h);
}

static void
test_width_and_height_are_unchanged_by_centering(void) {
    const recti_t r = ui_centered_rect(368, 240, 64, 0);
    TEST_ASSERT_EQUAL_INT_MESSAGE(240, r.w, "centering must not resize the content, only reposition it");
    TEST_ASSERT_EQUAL_INT(64, r.h);
}

void
suite_ui_centered_rect(void) {
    RUN_TEST(test_centered_across_a_spread_of_canvas_and_width_pairs);
    RUN_TEST(test_zero_margin_when_width_equals_canvas);
    RUN_TEST(test_y_and_h_pass_through_unchanged);
    RUN_TEST(test_width_and_height_are_unchanged_by_centering);
}

SUITE_REGISTER(suite_ui_centered_rect);

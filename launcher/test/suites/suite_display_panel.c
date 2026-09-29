/*
 * Portable suite: rounded panel geometry shared by shell layouts and tools.
 */

#include "suites.h"
#include "unity.h"

#include "display/display.h"

static void
test_the_top_row_is_hidden_by_the_corner_radius(void) {
    TEST_ASSERT_EQUAL_INT(40, display_panel_corner_inset(40, 448, 0));
}

static void
test_rows_past_the_corner_radius_hide_nothing(void) {
    TEST_ASSERT_EQUAL_INT(0, display_panel_corner_inset(40, 448, 40));
}

static void
test_the_inset_is_symmetric_at_both_panel_edges(void) {
    const int heights[] = {448, 368};
    for (size_t h = 0; h < sizeof heights / sizeof heights[0]; h++) {
        for (int row = 0; row < 40; row++) {
            TEST_ASSERT_EQUAL_INT(display_panel_corner_inset(40, heights[h], row),
                                  display_panel_corner_inset(40, heights[h], heights[h] - 1 - row));
        }
    }
}

static void
test_the_midpoint_follows_the_measured_circle(void) {
    TEST_ASSERT_EQUAL_INT(5, display_panel_corner_inset(40, 448, 20));
}

static void
test_the_inset_never_increases_away_from_the_corner(void) {
    int prior = display_panel_corner_inset(40, 448, 0);
    for (int row = 1; row < 40; row++) {
        const int inset = display_panel_corner_inset(40, 448, row);
        TEST_ASSERT_TRUE(inset <= prior);
        prior = inset;
    }
}

static void
test_a_zero_radius_has_no_inset(void) {
    TEST_ASSERT_EQUAL_INT(0, display_panel_corner_inset(0, 448, 0));
}

void
suite_display_panel(void) {
    RUN_TEST(test_the_top_row_is_hidden_by_the_corner_radius);
    RUN_TEST(test_rows_past_the_corner_radius_hide_nothing);
    RUN_TEST(test_the_inset_is_symmetric_at_both_panel_edges);
    RUN_TEST(test_the_midpoint_follows_the_measured_circle);
    RUN_TEST(test_the_inset_never_increases_away_from_the_corner);
    RUN_TEST(test_a_zero_radius_has_no_inset);
}

SUITE_REGISTER(suite_display_panel);

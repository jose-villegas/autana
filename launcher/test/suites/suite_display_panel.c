/*
 * Portable suite: rounded panel geometry shared by shell layouts and tools.
 */

#include "suites.h"
#include "unity.h"

#include "display/display.h"

static void
test_the_top_row_is_hidden_by_the_corner_radius(void) {
    TEST_ASSERT_EQUAL_INT(DISPLAY_PANEL_CORNER_RADIUS, display_panel_corner_inset(DISPLAY_PANEL_CORNER_RADIUS, 448, 0));
}

static void
test_rows_past_the_corner_radius_hide_nothing(void) {
    TEST_ASSERT_EQUAL_INT(0, display_panel_corner_inset(DISPLAY_PANEL_CORNER_RADIUS, 448, DISPLAY_PANEL_CORNER_RADIUS));
}

static void
test_the_inset_is_symmetric_at_both_panel_edges(void) {
    const int heights[] = {448, 368};
    for (size_t h = 0; h < sizeof heights / sizeof heights[0]; h++) {
        for (int row = 0; row < DISPLAY_PANEL_CORNER_RADIUS; row++) {
            TEST_ASSERT_EQUAL_INT(
                display_panel_corner_inset(DISPLAY_PANEL_CORNER_RADIUS, heights[h], row),
                display_panel_corner_inset(DISPLAY_PANEL_CORNER_RADIUS, heights[h], heights[h] - 1 - row));
        }
    }
}

static void
test_the_midpoint_follows_the_measured_circle(void) {
    TEST_ASSERT_EQUAL_INT(6, display_panel_corner_inset(DISPLAY_PANEL_CORNER_RADIUS, 448, 21));
}

void
suite_display_panel(void) {
    RUN_TEST(test_the_top_row_is_hidden_by_the_corner_radius);
    RUN_TEST(test_rows_past_the_corner_radius_hide_nothing);
    RUN_TEST(test_the_inset_is_symmetric_at_both_panel_edges);
    RUN_TEST(test_the_midpoint_follows_the_measured_circle);
}

SUITE_REGISTER(suite_display_panel);

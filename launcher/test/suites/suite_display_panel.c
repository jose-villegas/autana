/*
 * Portable suite: rounded panel geometry shared by shell layouts and tools.
 */

#include "suites.h"
#include "unity.h"

#include "display/display.h"
#include "gfx/gfx.h"

#include <math.h>

static void
test_the_top_row_is_hidden_by_the_corner_radius(void) {
    TEST_ASSERT_EQUAL_INT(40, display_panel_corner_inset(40, GFX_HEIGHT, 0));
}

static void
test_rows_past_the_corner_radius_hide_nothing(void) {
    TEST_ASSERT_EQUAL_INT(0, display_panel_corner_inset(40, GFX_HEIGHT, 40));
}

static void
test_the_inset_is_symmetric_at_both_panel_edges(void) {
    const int heights[] = {GFX_HEIGHT, GFX_WIDTH};
    for (size_t h = 0; h < sizeof heights / sizeof heights[0]; h++) {
        for (int row = 0; row < 40; row++) {
            TEST_ASSERT_EQUAL_INT(display_panel_corner_inset(40, heights[h], row),
                                  display_panel_corner_inset(40, heights[h], heights[h] - 1 - row));
        }
    }
}

static void
test_the_midpoint_follows_the_measured_circle(void) {
    TEST_ASSERT_EQUAL_INT(5, display_panel_corner_inset(40, GFX_HEIGHT, 20));
}

static void
test_the_inset_never_increases_away_from_the_corner(void) {
    int prior = display_panel_corner_inset(40, GFX_HEIGHT, 0);
    for (int row = 1; row < 40; row++) {
        const int inset = display_panel_corner_inset(40, GFX_HEIGHT, row);
        TEST_ASSERT_TRUE(inset <= prior);
        prior = inset;
    }
}

static void
test_a_zero_radius_has_no_inset(void) {
    TEST_ASSERT_EQUAL_INT(0, display_panel_corner_inset(0, GFX_HEIGHT, 0));
}

static void
test_panel_insets_match_float_chords(void) {
    const int heights[] = {GFX_WIDTH, GFX_HEIGHT};
    for (size_t i = 0; i < sizeof heights / sizeof heights[0]; i++) {
        const int height = heights[i];
        for (int radius = 0; radius <= GFX_HEIGHT; radius++) {
            for (int row = 0; row < height; row++) {
                const int edge = row < height - 1 - row ? row : height - 1 - row;
                const int up = radius - edge;
                const int want = edge >= radius ? 0 : radius - (int)lroundf(sqrtf((float)(radius * radius - up * up)));
                TEST_ASSERT_EQUAL_INT(want, display_panel_corner_inset(radius, height, row));
            }
        }
    }
}

void
suite_display_panel(void) {
    RUN_TEST(test_panel_insets_match_float_chords);
    RUN_TEST(test_the_top_row_is_hidden_by_the_corner_radius);
    RUN_TEST(test_rows_past_the_corner_radius_hide_nothing);
    RUN_TEST(test_the_inset_is_symmetric_at_both_panel_edges);
    RUN_TEST(test_the_midpoint_follows_the_measured_circle);
    RUN_TEST(test_the_inset_never_increases_away_from_the_corner);
    RUN_TEST(test_a_zero_radius_has_no_inset);
}

SUITE_REGISTER(suite_display_panel);

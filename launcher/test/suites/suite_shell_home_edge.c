#include "suites.h"
#include "unity.h"

#include "gfx/gfx.h"
#include "shell/shell_home_edge.h"
#include "ui/ui_transform.h"

#define QUARTER_TURN_COUNT 4

static void
test_home_edge_hugs_the_transformed_logical_bottom(void) {
    for (int quarter = 0; quarter < QUARTER_TURN_COUNT; quarter++) {
        const ui_transform_t transform = ui_transform_quarter_turn(quarter, GFX_WIDTH, GFX_HEIGHT);
        const bool swaps_axes = transform.a == 0;
        const int logical_w = swaps_axes ? GFX_HEIGHT : GFX_WIDTH;
        const int logical_h = swaps_axes ? GFX_WIDTH : GFX_HEIGHT;
        const mu_Rect bottom = {0, logical_h - GESTURE_HOME_ZONE_DEPTH, logical_w, GESTURE_HOME_ZONE_DEPTH};
        const mu_Rect physical = ui_transform_rect(transform, bottom);
        const bool spans_width = physical.x == 0 && physical.w == GFX_WIDTH;
        const bool spans_height = physical.y == 0 && physical.h == GFX_HEIGHT;
        const bool hugs_top = spans_width && physical.y == 0;
        const bool hugs_bottom = spans_width && physical.y + physical.h == GFX_HEIGHT;
        const bool hugs_left = spans_height && physical.x == 0;
        const bool hugs_right = spans_height && physical.x + physical.w == GFX_WIDTH;

        TEST_ASSERT_EQUAL_INT_MESSAGE(1, hugs_top + hugs_bottom + hugs_left + hugs_right,
                                      "the logical bottom must hug exactly one physical edge");
        gesture_edge_t expected;
        if (hugs_top) {
            expected = GESTURE_EDGE_TOP;
        } else if (hugs_bottom) {
            expected = GESTURE_EDGE_BOTTOM;
        } else if (hugs_left) {
            expected = GESTURE_EDGE_LEFT;
        } else {
            expected = GESTURE_EDGE_RIGHT;
        }
        TEST_ASSERT_EQUAL_INT(expected, shell_exit_edge_for_quarter(quarter));
    }
}

void
run_shell_home_edge_suite(void) {
    RUN_TEST(test_home_edge_hugs_the_transformed_logical_bottom);
}

SUITE_REGISTER(run_shell_home_edge_suite);

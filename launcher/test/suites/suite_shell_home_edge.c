#include "suites.h"
#include "unity.h"

#include "gfx/gfx.h"
#include "shell/shell_home_edge.h"
#include "ui/ui_transform.h"

#define QUARTER_TURN_COUNT 4

/* The physical edge the transformed logical bottom strip lies along, or -1
 * when it hugs none or more than one. */
static int
edge_under_logical_bottom(int quarter) {
    const ui_transform_t transform = ui_transform_quarter_turn(quarter, GFX_WIDTH, GFX_HEIGHT);
    const bool swaps_axes = transform.a == 0;
    const int logical_w = swaps_axes ? GFX_HEIGHT : GFX_WIDTH;
    const int logical_h = swaps_axes ? GFX_WIDTH : GFX_HEIGHT;
    const mu_Rect bottom = {0, logical_h - GESTURE_HOME_ZONE_DEPTH, logical_w, GESTURE_HOME_ZONE_DEPTH};
    const mu_Rect physical = ui_transform_rect(transform, bottom);
    const bool spans_width = physical.x == 0 && physical.w == GFX_WIDTH;
    const bool spans_height = physical.y == 0 && physical.h == GFX_HEIGHT;
    const bool hugs[4] = {
        [GESTURE_EDGE_TOP] = spans_width && physical.y == 0,
        [GESTURE_EDGE_BOTTOM] = spans_width && physical.y + physical.h == GFX_HEIGHT,
        [GESTURE_EDGE_LEFT] = spans_height && physical.x == 0,
        [GESTURE_EDGE_RIGHT] = spans_height && physical.x + physical.w == GFX_WIDTH,
    };
    int found = -1;
    for (int edge = 0; edge < 4; edge++) {
        if (hugs[edge]) {
            found = found == -1 ? edge : -2;
        }
    }
    return found;
}

static void
test_home_edge_hugs_the_transformed_logical_bottom(void) {
    for (int quarter = 0; quarter < QUARTER_TURN_COUNT; quarter++) {
        const int expected = edge_under_logical_bottom(quarter);
        TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(0, expected, "the logical bottom must hug exactly one physical edge");
        TEST_ASSERT_EQUAL_INT(expected, shell_exit_edge_for_quarter(quarter));
    }
}

void
run_shell_home_edge_suite(void) {
    RUN_TEST(test_home_edge_hugs_the_transformed_logical_bottom);
}

SUITE_REGISTER(run_shell_home_edge_suite);

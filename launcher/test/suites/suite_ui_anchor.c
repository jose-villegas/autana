/*
 * Portable suite: ui_anchor - rectangle anchors and pivots.
 */

#include "suites.h"
#include "unity.h"

#include "ui/ui_anchor.h"

static void
assert_rect(mu_Rect actual, int x, int y, int w, int h) {
    TEST_ASSERT_EQUAL_INT(x, actual.x);
    TEST_ASSERT_EQUAL_INT(y, actual.y);
    TEST_ASSERT_EQUAL_INT(w, actual.w);
    TEST_ASSERT_EQUAL_INT(h, actual.h);
}

static void
test_matching_presets_land_each_element_corner_or_centre(void) {
    const mu_Rect parent = {10, 20, 31, 23};
    const ui_anchor_t presets[] = {UI_ANCHOR_TOP_LEFT,    UI_ANCHOR_TOP,    UI_ANCHOR_TOP_RIGHT,
                                   UI_ANCHOR_LEFT,        UI_ANCHOR_CENTER, UI_ANCHOR_RIGHT,
                                   UI_ANCHOR_BOTTOM_LEFT, UI_ANCHOR_BOTTOM, UI_ANCHOR_BOTTOM_RIGHT};
    const int xs[] = {10, 22, 34, 10, 22, 34, 10, 22, 34};
    const int ys[] = {20, 20, 20, 29, 29, 29, 38, 38, 38};

    for (int i = 0; i < 9; i++) {
        assert_rect(ui_anchor_rect(parent, presets[i], presets[i], 0, 0, 7, 5), xs[i], ys[i], 7, 5);
    }
}

static void
test_different_pivots_share_the_same_parent_anchor(void) {
    const mu_Rect parent = {10, 20, 31, 23};

    assert_rect(ui_anchor_rect(parent, UI_ANCHOR_BOTTOM_RIGHT, UI_ANCHOR_BOTTOM_RIGHT, 0, 0, 7, 5), 34, 38, 7, 5);
    assert_rect(ui_anchor_rect(parent, UI_ANCHOR_BOTTOM_RIGHT, UI_ANCHOR_TOP_LEFT, 0, 0, 7, 5), 41, 43, 7, 5);
}

static void
test_offsets_move_the_pivot_from_the_anchor(void) {
    assert_rect(ui_anchor_rect((mu_Rect){10, 20, 31, 23}, UI_ANCHOR_LEFT, UI_ANCHOR_RIGHT, 3, -4, 7, 5), 6, 25, 7, 5);
}

static void
test_centre_rounds_like_subtract_then_divide(void) {
    assert_rect(ui_anchor_rect((mu_Rect){0, 0, 10, 10}, UI_ANCHOR_CENTER, UI_ANCHOR_CENTER, 0, 0, 5, 5), 2, 2, 5, 5);
}

static void
test_inset_shrinks_each_edge_independently(void) {
    assert_rect(ui_rect_inset((mu_Rect){10, 20, 31, 23}, 1, 2, 3, 4), 11, 22, 27, 17);
}

static void
test_resolve_then_turn_maps_top_centre_at_each_quarter(void) {
    static const mu_Rect expected[] = {{124, 10, 120, 4}, {354, 164, 4, 120}, {124, 434, 120, 4}, {10, 164, 4, 120}};

    for (int turn = 0; turn < 4; turn++) {
        const int logical_w = (turn % 2 == 0) ? 368 : 448;
        const int logical_h = (turn % 2 == 0) ? 448 : 368;
        const mu_Rect upright =
            ui_anchor_rect((mu_Rect){0, 0, logical_w, logical_h}, UI_ANCHOR_TOP, UI_ANCHOR_TOP, 0, 10, 120, 4);
        const mu_Rect mapped = ui_transform_rect(ui_transform_quarter_turn(turn, 368, 448), upright);

        assert_rect(mapped, expected[turn].x, expected[turn].y, expected[turn].w, expected[turn].h);
    }
}

void
suite_ui_anchor(void) {
    RUN_TEST(test_matching_presets_land_each_element_corner_or_centre);
    RUN_TEST(test_different_pivots_share_the_same_parent_anchor);
    RUN_TEST(test_offsets_move_the_pivot_from_the_anchor);
    RUN_TEST(test_centre_rounds_like_subtract_then_divide);
    RUN_TEST(test_inset_shrinks_each_edge_independently);
    RUN_TEST(test_resolve_then_turn_maps_top_centre_at_each_quarter);
}

SUITE_REGISTER(suite_ui_anchor);

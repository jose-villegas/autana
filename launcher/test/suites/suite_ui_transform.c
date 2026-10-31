/*
 * Portable suite: viewport quarter turns, axis checks, text and icon adapters.
 * VIEW_W/VIEW_H are independent of gfx's dimensions so a wrong panel size
 * cannot agree with the same wrong constant in the fixture.
 */

#include <stdbool.h>

#include "icon_walk.h"
#include "suites.h"
#include "unity.h"

#include "ui/layout/ui_layout.h"
#include "ui/layout/ui_transform.h"

#define VIEW_W 368
#define VIEW_H 448

/*
 * Quarter turns: where the viewport's own corners land
 *
 * The domain each turn expects is the LOGICAL canvas: VIEW_H x VIEW_W for
 * an odd turn, VIEW_W x VIEW_H for an even one, exactly what ui_width()/
 * ui_height() would report (see ui.h). Expected corners are the ones
 * ui_transform_quarter_turn()'s own doc comment claims: each corner moves to
 * the next one clockwise, once per quarter turn.
 */

static void
check_point(affine2x_t t, int x, int y, int ex, int ey, const char* msg) {
    const vec2i_t mapped = affine2x_map_pixel(&t, (vec2i_t){x, y});
    TEST_ASSERT_EQUAL_INT_MESSAGE(ex, mapped.x, msg);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ey, mapped.y, msg);
}

static void
test_turn_0_leaves_corners_where_they_are(void) {
    const affine2x_t t = ui_transform_quarter_turn(0, VIEW_W, VIEW_H);
    check_point(t, 0, 0, 0, 0, "turn 0: top-left");
    check_point(t, VIEW_W, 0, VIEW_W, 0, "turn 0: top-right");
    check_point(t, 0, VIEW_H, 0, VIEW_H, "turn 0: bottom-left");
    check_point(t, VIEW_W, VIEW_H, VIEW_W, VIEW_H, "turn 0: bottom-right");
}

static void
test_turn_1_rotates_corners_one_step_clockwise(void) {
    /* Domain is VIEW_H wide, VIEW_W tall, the swap odd turns imply. */
    const affine2x_t t = ui_transform_quarter_turn(1, VIEW_W, VIEW_H);
    check_point(t, 0, 0, VIEW_W, 0, "turn 1: logical top-left -> physical top-right");
    check_point(t, VIEW_H, 0, VIEW_W, VIEW_H, "turn 1: logical top-right -> physical bottom-right");
    check_point(t, 0, VIEW_W, 0, 0, "turn 1: logical bottom-left -> physical top-left");
    check_point(t, VIEW_H, VIEW_W, 0, VIEW_H, "turn 1: logical bottom-right -> physical bottom-left");
}

static void
test_turn_2_rotates_corners_two_steps(void) {
    const affine2x_t t = ui_transform_quarter_turn(2, VIEW_W, VIEW_H);
    check_point(t, 0, 0, VIEW_W, VIEW_H, "turn 2: top-left -> bottom-right");
    check_point(t, VIEW_W, 0, 0, VIEW_H, "turn 2: top-right -> bottom-left");
    check_point(t, 0, VIEW_H, VIEW_W, 0, "turn 2: bottom-left -> top-right");
    check_point(t, VIEW_W, VIEW_H, 0, 0, "turn 2: bottom-right -> top-left");
}

static void
test_turn_3_rotates_corners_three_steps_clockwise(void) {
    const affine2x_t t = ui_transform_quarter_turn(3, VIEW_W, VIEW_H);
    check_point(t, 0, 0, 0, VIEW_H, "turn 3: logical top-left -> physical bottom-left");
    check_point(t, VIEW_H, 0, 0, 0, "turn 3: logical top-right -> physical top-left");
    check_point(t, 0, VIEW_W, VIEW_W, VIEW_H, "turn 3: logical bottom-left -> physical bottom-right");
    check_point(t, VIEW_H, VIEW_W, VIEW_W, 0, "turn 3: logical bottom-right -> physical top-right");
}

/* Unmapping a physical pixel returns the logical one each turn maps onto it */

static void
test_unmap_pixel_inverts_every_quarter_turn(void) {
    for (int turn = 0; turn < 4; turn++) {
        const affine2x_t t = ui_transform_quarter_turn(turn, VIEW_W, VIEW_H);
        const vec2i_t logical = {VIEW_W / 3, VIEW_H / 5};
        const vec2i_t back = ui_transform_unmap_pixel(t, affine2x_map_pixel(&t, logical));
        TEST_ASSERT_EQUAL_INT_MESSAGE(logical.x, back.x, "unmap(map(p)).x == p.x");
        TEST_ASSERT_EQUAL_INT_MESSAGE(logical.y, back.y, "unmap(map(p)).y == p.y");
    }
}

static void
test_unmap_pixel_leaves_the_pixel_when_the_transform_is_singular(void) {
    affine2x_t singular = ui_transform_quarter_turn(1, VIEW_W, VIEW_H);
    singular.y_axis = singular.x_axis;
    const vec2i_t physical = {VIEW_W / 3, VIEW_H / 5};
    const vec2i_t back = ui_transform_unmap_pixel(singular, physical);
    TEST_ASSERT_EQUAL_INT(physical.x, back.x);
    TEST_ASSERT_EQUAL_INT(physical.y, back.y);
}

/* A rect stays axis-aligned and inside the viewport after any quarter turn */

static void
assert_rect_fits(affine2x_t t, recti_t r, const char* msg) {
    const recti_t out = ui_transform_rect(t, r);

    TEST_ASSERT_TRUE_MESSAGE(out.w > 0 && out.h > 0, msg);
    TEST_ASSERT_TRUE_MESSAGE(out.x >= 0 && out.y >= 0, msg);
    TEST_ASSERT_TRUE_MESSAGE(out.x + out.w <= VIEW_W, msg);
    TEST_ASSERT_TRUE_MESSAGE(out.y + out.h <= VIEW_H, msg);
}

static void
test_a_mapped_rect_stays_axis_aligned_and_inside_the_viewport(void) {
    const recti_t even_domain_rect = {16, 80, 100, 64};
    const recti_t odd_domain_rect = {16, 80, 100, 64}; /* fits both domains */

    assert_rect_fits(ui_transform_quarter_turn(0, VIEW_W, VIEW_H), even_domain_rect,
                     "turn 0 rect must land inside the viewport");
    assert_rect_fits(ui_transform_quarter_turn(1, VIEW_W, VIEW_H), odd_domain_rect,
                     "turn 1 rect must land inside the viewport");
    assert_rect_fits(ui_transform_quarter_turn(2, VIEW_W, VIEW_H), even_domain_rect,
                     "turn 2 rect must land inside the viewport");
    assert_rect_fits(ui_transform_quarter_turn(3, VIEW_W, VIEW_H), odd_domain_rect,
                     "turn 3 rect must land inside the viewport");
}

static void
test_a_mapped_rect_swaps_width_and_height_on_an_odd_turn(void) {
    const recti_t r = {16, 80, 100, 64};
    const recti_t t1 = ui_transform_rect(ui_transform_quarter_turn(1, VIEW_W, VIEW_H), r);
    const recti_t t3 = ui_transform_rect(ui_transform_quarter_turn(3, VIEW_W, VIEW_H), r);

    TEST_ASSERT_EQUAL_INT_MESSAGE(r.h, t1.w,
                                  "a quarter turn is a pure rotation - it must swap w and h, not scale them");
    TEST_ASSERT_EQUAL_INT(r.w, t1.h);
    TEST_ASSERT_EQUAL_INT(r.h, t3.w);
    TEST_ASSERT_EQUAL_INT(r.w, t3.h);
}

/*
 * A synthetic 4x4 "L": the minimum shape that is NOT symmetric under any
 * quarter turn, so a bug mapping only the enclosing box produces runs a
 * symmetric glyph could never distinguish from correct ones. Expected
 * geometry is hand-derived from ui_transform_quarter_turn()'s matrices.
 *
 * Its content box is 3 wide and 4 tall, which at a 4x4 box is scale 1 with
 * a zero origin, so LOCAL run coordinates below equal native bitmap ones
 * and no centring arithmetic stands in front of the transform math.
 */

static const uint8_t icon_l_rows[4] = {
    0x80, /* 1000: col 0 */
    0x80, /* 1000: col 0 */
    0x80, /* 1000: col 0 */
    0xE0, /* 1110: cols 0,1,2 */
};
static const image_t icon_l = {
    .pixels = (void*)icon_l_rows, .format = IMAGE_MONO1, .width = 4, .height = 4, .pitch = 8 / CHAR_BIT};

/* box = (10, 20, 4, 4), fixed across every quarter below. Expected physical
 * rects were mapped BY HAND, corner to corner, from
 * ui_transform_quarter_turn()'s own matrices (see that function's
 * comment), not through ui_transform_rect(), so a bug shared between it
 * and the function under test cannot hide behind agreement between the
 * two. The cross-check test below covers that angle separately. */
static void
assert_icon_blocks_at_quarter(int quarter, const recti_t* expected, const char* msg) {
    const affine2x_t t = ui_transform_quarter_turn(quarter, VIEW_W, VIEW_H);
    const recti_t box = {10, 20, 4, 4};
    recti_t runs[4];
    icon_test_collect_t ic = {.blocks = runs, .count = 0, .cap = 4};

    ui_transform_icon_blocks(t, &icon_l, box, icon_test_collect, &ic);

    TEST_ASSERT_EQUAL_INT_MESSAGE(4, ic.count, msg);
    for (int i = 0; i < 4; i++) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(expected[i].x, ic.blocks[i].x, msg);
        TEST_ASSERT_EQUAL_INT_MESSAGE(expected[i].y, ic.blocks[i].y, msg);
        TEST_ASSERT_EQUAL_INT_MESSAGE(expected[i].w, ic.blocks[i].w, msg);
        TEST_ASSERT_EQUAL_INT_MESSAGE(expected[i].h, ic.blocks[i].h, msg);
    }
}

static void
test_icon_blocks_at_turn_0_are_unrotated(void) {
    const recti_t expected[4] = {
        {10, 20, 1, 1},
        {10, 21, 1, 1},
        {10, 22, 1, 1},
        {10, 23, 3, 1},
    };
    assert_icon_blocks_at_quarter(0, expected, "turn 0: runs must match the box exactly");
}

static void
test_icon_blocks_at_turn_1_rotate_one_step_clockwise(void) {
    const recti_t expected[4] = {
        {347, 10, 1, 1},
        {346, 10, 1, 1},
        {345, 10, 1, 1},
        {344, 10, 1, 3},
    };
    assert_icon_blocks_at_quarter(1, expected,
                                  "turn 1: each run must rotate with the glyph, not stay upright inside a rotated box");
}

static void
test_icon_blocks_at_turn_2_rotate_two_steps(void) {
    const recti_t expected[4] = {
        {357, 427, 1, 1},
        {357, 426, 1, 1},
        {357, 425, 1, 1},
        {355, 424, 3, 1},
    };
    assert_icon_blocks_at_quarter(2, expected, "turn 2: runs must rotate a half turn");
}

static void
test_icon_blocks_at_turn_3_rotate_three_steps_clockwise(void) {
    const recti_t expected[4] = {
        {20, 437, 1, 1},
        {21, 437, 1, 1},
        {22, 437, 1, 1},
        {23, 435, 1, 3},
    };
    assert_icon_blocks_at_quarter(3, expected,
                                  "turn 3: each run must rotate with the glyph, not stay upright inside a rotated box");
}

/* CROSS-CHECK, independent of the hand-derived expectations above: every
 * run, treated as its own rect and mapped straight through
 * ui_transform_rect() (the function every other MU_COMMAND_* already
 * trusts) must equal what ui_transform_icon_blocks() produced for it. */
static void
test_icon_blocks_match_ui_transform_rect_run_by_run(void) {
    const recti_t box = {10, 20, 4, 4};
    const recti_t local[4] = {
        {0, 0, 1, 1},
        {0, 1, 1, 1},
        {0, 2, 1, 1},
        {0, 3, 3, 1},
    };

    for (int quarter = 0; quarter < 4; quarter++) {
        const affine2x_t t = ui_transform_quarter_turn(quarter, VIEW_W, VIEW_H);
        recti_t runs[4];
        icon_test_collect_t ic = {.blocks = runs, .count = 0, .cap = 4};
        ui_transform_icon_blocks(t, &icon_l, box, icon_test_collect, &ic);

        TEST_ASSERT_EQUAL_INT(4, ic.count);
        for (int i = 0; i < 4; i++) {
            const recti_t truth =
                ui_transform_rect(t, (recti_t){box.x + local[i].x, box.y + local[i].y, local[i].w, local[i].h});
            TEST_ASSERT_EQUAL_INT(truth.x, ic.blocks[i].x);
            TEST_ASSERT_EQUAL_INT(truth.y, ic.blocks[i].y);
            TEST_ASSERT_EQUAL_INT(truth.w, ic.blocks[i].w);
            TEST_ASSERT_EQUAL_INT(truth.h, ic.blocks[i].h);
        }
    }
}

/*
 * A square font (cell_w == cell_h) can never expose a cell_w/cell_h mixup,
 * since whichever a bug reads the number comes out the same. glyph0_font is
 * deliberately non-square (6 x 10) AND proportional (advances 3/5/4/6, none
 * equal to cell_w); its atlas is unread and left zeroed.
 */

static const uint8_t glyph0_atlas[4 * 10] = {0}; /* 4 glyphs * cell_h rows, unread */
static const uint8_t glyph0_advance[4] = {3, 5, 4, 6};
static const draw_font_t glyph0_font = {
    .atlas = glyph0_atlas,
    .cell_w = 6,
    .cell_h = 10,
    .first = (uint8_t)'A',
    .count = 4,
    .advance = glyph0_advance,
};

/* The ground truth ui_text_glyph0_origin()'s own comment claims it matches:
 * glyph 0's OWN logical cell (cell_w x cell_h, at the string's logical
 * origin) mapped straight through ui_transform_rect() (the one function
 * every other MU_COMMAND_* (_RECT, _ICON, _CLIP) already trusts without
 * dispute) must describe exactly where the function under test says
 * glyph 0 belongs, at every quarter, not just the two (2 and 3) that get an
 * explicit correction. */
static void
assert_glyph0_matches_ground_truth(int logical_x, int logical_y, const char* text, int quarter, const char* msg) {
    const affine2x_t t = ui_transform_quarter_turn(quarter, VIEW_W, VIEW_H);
    const int tw = draw_font_text_width(&glyph0_font, text, -1, 1);
    const int th = draw_font_height(&glyph0_font, 1);
    const recti_t box = ui_transform_rect(t, (recti_t){logical_x, logical_y, tw, th});

    const vec2i_t origin = ui_text_glyph0_origin(&glyph0_font, box, quarter, 1);

    const recti_t truth = ui_transform_rect(t, (recti_t){logical_x, logical_y, glyph0_font.cell_w, glyph0_font.cell_h});

    TEST_ASSERT_EQUAL_INT_MESSAGE(truth.x, origin.x, msg);
    TEST_ASSERT_EQUAL_INT_MESSAGE(truth.y, origin.y, msg);
}

static void
test_glyph0_origin_matches_ground_truth_at_every_quarter(void) {
    for (int quarter = 0; quarter < 4; quarter++) {
        assert_glyph0_matches_ground_truth(20, 30, "ABCD", quarter,
                                           "glyph 0's origin must land exactly where mapping its own "
                                           "cell independently through ui_transform_rect() says it "
                                           "should, for a non-square proportional font");
    }

    /* And again with strings of every length 1..4, including the single-
     * glyph case (no OTHER glyph to walk past, the case most likely to
     * expose an off-by-one in whichever correction is applied). */
    for (int len = 1; len <= 4; len++) {
        for (int quarter = 0; quarter < 4; quarter++) {
            assert_glyph0_matches_ground_truth(20, 30, "ABCD" + (4 - len), quarter,
                                               "same check, at a shorter string length");
        }
    }
}

/* Turns 0 and 1 walk FORWARD, so the box's own corner already IS glyph 0's
 * origin: no correction applied, checked directly against the box rather
 * than against ui_transform_rect() ground truth (which the test above
 * already covers) so THIS test states the "no correction" contract in the
 * plainest possible terms. */
static void
test_glyph0_origin_needs_no_correction_at_turn_0_or_1(void) {
    const recti_t box = {50, 60, 30, 12};
    vec2i_t origin = ui_text_glyph0_origin(&glyph0_font, box, 0, 1);
    TEST_ASSERT_EQUAL_INT_MESSAGE(box.x, origin.x, "turn 0: origin is box.x");
    TEST_ASSERT_EQUAL_INT_MESSAGE(box.y, origin.y, "turn 0: origin is box.y");

    origin = ui_text_glyph0_origin(&glyph0_font, box, 1, 1);
    TEST_ASSERT_EQUAL_INT_MESSAGE(box.x, origin.x, "turn 1: origin is box.x");
    TEST_ASSERT_EQUAL_INT_MESSAGE(box.y, origin.y, "turn 1: origin is box.y");
}

/* Swapping cell_w for cell_h at quarter 3 LOOKS plausible, since quarter 3
 * corrects box.h where quarter 2 corrects box.w. No mutation is needed to
 * show this test would catch it: 6 and 10 make the two formulas disagree,
 * so the real answer not being the cell_h one is the evidence. */
static void
test_glyph0_origin_at_turn_3_is_not_the_cell_h_mistake(void) {
    const int quarter = 3;
    const affine2x_t t = ui_transform_quarter_turn(quarter, VIEW_W, VIEW_H);
    const int tw = draw_font_text_width(&glyph0_font, "ABCD", -1, 1);
    const int th = draw_font_height(&glyph0_font, 1);
    const recti_t box = ui_transform_rect(t, (recti_t){20, 30, tw, th});

    const vec2i_t origin = ui_text_glyph0_origin(&glyph0_font, box, quarter, 1);

    const int wrong_my_using_cell_h = box.y + box.h - glyph0_font.cell_h;
    TEST_ASSERT_NOT_EQUAL_MESSAGE(wrong_my_using_cell_h, origin.y,
                                  "cell_w (6) != cell_h (10) in this font - a function that read "
                                  "cell_h here instead of cell_w would land on `wrong_my_using_"
                                  "cell_h`, not on the real answer");
}

/* Composition and the quarter turn a transform represents */

static void
test_quarter_reports_the_turn_each_matrix_represents(void) {
    TEST_ASSERT_EQUAL_INT(0, ui_transform_quarter(affine2x_identity()));
    TEST_ASSERT_EQUAL_INT(0, ui_transform_quarter(ui_transform_quarter_turn(0, VIEW_W, VIEW_H)));
    TEST_ASSERT_EQUAL_INT(1, ui_transform_quarter(ui_transform_quarter_turn(1, VIEW_W, VIEW_H)));
    TEST_ASSERT_EQUAL_INT(2, ui_transform_quarter(ui_transform_quarter_turn(2, VIEW_W, VIEW_H)));
    TEST_ASSERT_EQUAL_INT(3, ui_transform_quarter(ui_transform_quarter_turn(3, VIEW_W, VIEW_H)));
}

/* The backend-vs-type boundary */

static void
test_axis_preserving_accepts_identity_and_every_quarter_turn(void) {
    TEST_ASSERT_TRUE(ui_transform_is_axis_preserving(affine2x_identity()));
    for (int turn = 0; turn < 4; turn++) {
        TEST_ASSERT_TRUE_MESSAGE(ui_transform_is_axis_preserving(ui_transform_quarter_turn(turn, VIEW_W, VIEW_H)),
                                 "every quarter turn is exactly what this backend can draw");
    }
}

static void
test_axis_preserving_accepts_translation(void) {
    const affine2x_t translate = {{MATHX_ONE, 0}, {0, MATHX_ONE}, {40 * MATHX_ONE, -12 * MATHX_ONE}};
    TEST_ASSERT_TRUE_MESSAGE(ui_transform_is_axis_preserving(translate),
                             "a pure translation keeps every rect axis-aligned");
}

static void
test_axis_preserving_accepts_integer_scale(void) {
    const affine2x_t scale_up = {{2 * MATHX_ONE, 0}, {0, 2 * MATHX_ONE}, {0, 0}};
    const affine2x_t scale_and_swap = /* an integer-scaled quarter turn */
        {{0, 3 * MATHX_ONE}, {-3 * MATHX_ONE, 0}, {100, 0}};
    TEST_ASSERT_TRUE_MESSAGE(ui_transform_is_axis_preserving(scale_up),
                             "an integer scale keeps every rect axis-aligned");
    TEST_ASSERT_TRUE_MESSAGE(ui_transform_is_axis_preserving(scale_and_swap),
                             "a scaled quarter turn is still axis-preserving - it is the "
                             "magnitude of a/b/c/d that carries the scale, the SIGN pattern "
                             "that carries the rotation, and this checks only the pattern");
}

static void
test_axis_preserving_rejects_a_shear(void) {
    /* c and d unrotated, but b is nonzero too: the y-axis now has a
     * component along x as well as y, which is exactly a shear. */
    const affine2x_t shear = {{MATHX_ONE, MATHX_ONE / 2}, {0, MATHX_ONE}, {0, 0}};
    TEST_ASSERT_FALSE_MESSAGE(ui_transform_is_axis_preserving(shear),
                              "a shear must be rejected - it turns an axis-aligned rect into a "
                              "parallelogram, which draw_rect() cannot draw");
}

static void
test_axis_preserving_rejects_a_non_90_degree_rotation(void) {
    /* All four entries nonzero: neither axis maps cleanly onto x or y. This
     * is not a real rotation matrix (its rows are not unit length), but the
     * classifier only needs to see that both columns are mixed to reject
     * it, which is exactly what a genuine 45 degree rotation would also
     * present. */
    const affine2x_t skew_rotation = {{MATHX_ONE, MATHX_ONE / 2}, {MATHX_ONE / 2, MATHX_ONE}, {0, 0}};
    TEST_ASSERT_FALSE_MESSAGE(ui_transform_is_axis_preserving(skew_rotation),
                              "a rotation that is not a multiple of 90 degrees must be rejected - "
                              "draw_text_turned() only has four quarters to offer it");
}

static void
test_quarter_turns_reads_back_the_turn_a_transform_was_built_from(void) {
    for (int turn = -4; turn <= 7; turn++) {
        const affine2x_t t = ui_transform_quarter_turn(turn, VIEW_W, VIEW_H);
        TEST_ASSERT_EQUAL_INT(((turn % 4) + 4) % 4, ui_transform_quarter_turns(t));
    }
    TEST_ASSERT_EQUAL_INT(0, ui_transform_quarter_turns(affine2x_identity()));
}

static void
test_rect_conversions_preserve_signed_coordinates_and_sizes(void) {
    const ui_layout_rect_t authored = {INT16_MIN, INT16_MAX, INT16_MAX, 0};
    const recti_t rect = ui_layout_rect_to_recti(authored);
    TEST_ASSERT_EQUAL_INT(INT16_MIN, rect.x);
    TEST_ASSERT_EQUAL_INT(INT16_MAX, rect.y);
    TEST_ASSERT_EQUAL_INT(INT16_MAX, rect.w);
    TEST_ASSERT_EQUAL_INT(0, rect.h);
}

void
suite_ui_transform(void) {
    RUN_TEST(test_rect_conversions_preserve_signed_coordinates_and_sizes);
    RUN_TEST(test_turn_0_leaves_corners_where_they_are);
    RUN_TEST(test_turn_1_rotates_corners_one_step_clockwise);
    RUN_TEST(test_turn_2_rotates_corners_two_steps);
    RUN_TEST(test_turn_3_rotates_corners_three_steps_clockwise);
    RUN_TEST(test_unmap_pixel_inverts_every_quarter_turn);
    RUN_TEST(test_unmap_pixel_leaves_the_pixel_when_the_transform_is_singular);
    RUN_TEST(test_a_mapped_rect_stays_axis_aligned_and_inside_the_viewport);
    RUN_TEST(test_a_mapped_rect_swaps_width_and_height_on_an_odd_turn);
    RUN_TEST(test_icon_blocks_at_turn_0_are_unrotated);
    RUN_TEST(test_icon_blocks_at_turn_1_rotate_one_step_clockwise);
    RUN_TEST(test_icon_blocks_at_turn_2_rotate_two_steps);
    RUN_TEST(test_icon_blocks_at_turn_3_rotate_three_steps_clockwise);
    RUN_TEST(test_icon_blocks_match_ui_transform_rect_run_by_run);
    RUN_TEST(test_glyph0_origin_matches_ground_truth_at_every_quarter);
    RUN_TEST(test_glyph0_origin_needs_no_correction_at_turn_0_or_1);
    RUN_TEST(test_glyph0_origin_at_turn_3_is_not_the_cell_h_mistake);
    RUN_TEST(test_quarter_reports_the_turn_each_matrix_represents);
    RUN_TEST(test_axis_preserving_accepts_identity_and_every_quarter_turn);
    RUN_TEST(test_axis_preserving_accepts_translation);
    RUN_TEST(test_axis_preserving_accepts_integer_scale);
    RUN_TEST(test_axis_preserving_rejects_a_shear);
    RUN_TEST(test_axis_preserving_rejects_a_non_90_degree_rotation);
    RUN_TEST(test_quarter_turns_reads_back_the_turn_a_transform_was_built_from);
}

SUITE_REGISTER(suite_ui_transform);

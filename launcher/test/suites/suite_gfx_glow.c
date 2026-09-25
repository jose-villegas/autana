/*
 * Portable suite: gfx_glow.h - a curve drawn as light by true distance.
 * Drives gfx_glow_draw_posed_rows() into a small buffer of its own.
 */

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "suites.h"
#include "unity.h"

#ifdef DEVICE_BUILD
#include "esp_heap_caps.h"
#endif

#include "gfx/gfx_glow.h"
#include "util/trig.h"

#define PANEL_W   96
#define PANEL_H   80
#define RADIUS    8
#define UNTOUCHED ((gfx_color_t)0x1234)

static gfx_color_t* pixels;
static gfx_color_t* truth;
static gfx_glow_style_t* style;
static int16_t heights[PANEL_W];
static int16_t posed_spans[4][PANEL_W];
static int16_t lit_lo[PANEL_H];
static int16_t lit_hi[PANEL_H];
static int posed_trail;
static const gfx_glow_map_t* posed_map;

static void
fixture_begin(void) {
    pixels = malloc(sizeof(gfx_color_t) * PANEL_W * PANEL_H);
#ifdef DEVICE_BUILD
    truth = heap_caps_malloc(sizeof(gfx_color_t) * PANEL_W * PANEL_H, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    truth = malloc(sizeof(gfx_color_t) * PANEL_W * PANEL_H);
#endif
    style = malloc(sizeof *style);
    TEST_ASSERT_NOT_NULL(truth);
    TEST_ASSERT_NOT_NULL(pixels);
    TEST_ASSERT_NOT_NULL(style);
    for (int i = 0; i < PANEL_W * PANEL_H; i++) {
        pixels[i] = UNTOUCHED;
    }
    gfx_glow_style_set(style, RADIUS, 2, 0xFFFFFF, 0x30C0FF);
}

static void
fixture_end(void) {
    free(pixels);
    free(truth);
    free(style);
    pixels = NULL;
    truth = NULL;
    style = NULL;
}

static gfx_target_t
whole_panel(void) {
    return (gfx_target_t){pixels, 0, PANEL_H, PANEL_W};
}

static gfx_glow_field_t
posed_field_count(int count) {
    gfx_glow_field_t field = {.span_lo = posed_spans[0],
                              .span_hi = posed_spans[1],
                              .reach_lo = posed_spans[2],
                              .reach_hi = posed_spans[3],
                              .count = count};
    gfx_glow_field_prepare(&field, heights, style);
    return field;
}

static gfx_glow_box_t
draw_curve(gfx_target_t target, int count, int view_h, gfx_glow_pose_t pose, int clip_x0, int clip_y0, int clip_x1,
           int clip_y1, int row0, int row1) {
    const gfx_glow_field_t field = posed_field_count(count);
    memset(lit_lo, 0, sizeof lit_lo);
    memset(lit_hi, 0, sizeof lit_hi);
    return gfx_glow_draw_posed_rows(target, clip_x0, clip_y0, clip_x1, clip_y1, PANEL_W, PANEL_H, &field, NULL, view_h,
                                    pose, row0, row1, lit_lo, lit_hi, 0, style);
}

static gfx_glow_box_t
draw_identity(gfx_target_t target) {
    return draw_curve(target, PANEL_W, PANEL_H, (gfx_glow_pose_t){0, GFX_GLOW_POSE_ONE}, 0, 0, PANEL_W, PANEL_H, 0,
                      PANEL_H);
}

static bool
lit(int x, int y) {
    const gfx_color_t c = pixels[y * PANEL_W + x];
    return c != UNTOUCHED && c != GFX_RGB(0x000000);
}

static int
brightness(gfx_color_t c) {
    const uint32_t rgb = gfx_color_rgb888(c);
    return (int)((rgb >> 16) + ((rgb >> 8) & 0xFF) + (rgb & 0xFF));
}

static void
flat_curve(int row) {
    for (int x = 0; x < PANEL_W; x++) {
        heights[x] = (int16_t)(row * GFX_GLOW_ONE + GFX_GLOW_ONE / 2);
    }
}

static void
test_ramp_index_runs_from_the_curve_to_the_rim_without_a_square_root(void) {
    TEST_ASSERT_EQUAL_UINT32(0, gfx_glow_ramp_index(0));
    TEST_ASSERT_EQUAL_UINT32(GFX_GLOW_RAMP_SIZE - 1, gfx_glow_ramp_index(4095));
    /* (d/r)^2 = 1/4 is d/r = 1/2. */
    TEST_ASSERT_INT_WITHIN(1, GFX_GLOW_RAMP_SIZE / 2, (int)gfx_glow_ramp_index(1024));
    uint32_t last = 0;
    for (uint32_t u = 0; u < 4096; u++) {
        const uint32_t index = gfx_glow_ramp_index(u);
        TEST_ASSERT_TRUE(index >= last && index < GFX_GLOW_RAMP_SIZE);
        last = index;
    }
}

static void
test_every_phase_fades_from_the_core_colour_to_black(void) {
    fixture_begin();
    for (int phase = 0; phase < GFX_GLOW_PHASES; phase++) {
        TEST_ASSERT_EQUAL_HEX16(GFX_RGB(0xFFFFFF), style->ramp[phase][0]);
        TEST_ASSERT_EQUAL_HEX16(GFX_RGB(0x000000), style->ramp[phase][GFX_GLOW_RAMP_SIZE - 1]);
        for (int i = 1; i < GFX_GLOW_RAMP_SIZE; i++) {
            TEST_ASSERT_TRUE(brightness(style->ramp[phase][i]) <= brightness(style->ramp[phase][i - 1]));
        }
    }
    fixture_end();
}

static void
test_phases_round_one_colour_at_different_thresholds(void) {
    fixture_begin();
    int differing = 0;
    for (int i = 0; i < GFX_GLOW_RAMP_SIZE; i++) {
        differing += style->ramp[0][i] != style->ramp[GFX_GLOW_PHASES - 1][i];
        TEST_ASSERT_TRUE(brightness(style->ramp[0][i]) <= brightness(style->ramp[GFX_GLOW_PHASES - 1][i]));
    }
    TEST_ASSERT_GREATER_THAN_INT(GFX_GLOW_RAMP_SIZE / 2, differing);
    fixture_end();
}

static void
test_a_flat_curve_lights_its_radius_and_no_further(void) {
    fixture_begin();
    flat_curve(40);
    draw_identity(whole_panel());
    for (int x = 0; x < PANEL_W; x++) {
        TEST_ASSERT_TRUE(lit(x, 40));
        TEST_ASSERT_TRUE(lit(x, 40 - (RADIUS - 2)) && lit(x, 40 + (RADIUS - 2)));
        TEST_ASSERT_FALSE(lit(x, 40 - RADIUS - 1) || lit(x, 40 + RADIUS + 1));
        TEST_ASSERT_TRUE(brightness(pixels[40 * PANEL_W + x]) > brightness(pixels[37 * PANEL_W + x]));
    }
    fixture_end();
}

/* The case a slope-scaled vertical falloff gets wrong - see gfx_glow.h. */
static void
test_a_cliff_lights_nothing_far_above_the_plateau(void) {
    fixture_begin();
    for (int x = 0; x < PANEL_W; x++) {
        const int row = x < 40 ? 20 : (x < 48 ? 20 + (x - 40) * 5 : 60);
        heights[x] = (int16_t)(row * GFX_GLOW_ONE);
    }
    draw_identity(whole_panel());
    for (int x = 36; x < 52; x++) {
        for (int y = 0; y < 20 - RADIUS - 1; y++) {
            TEST_ASSERT_FALSE(lit(x, y));
        }
    }
    /* And the cliff face itself is lit to its radius on both sides. */
    TEST_ASSERT_TRUE(lit(44, 40) && lit(44 - 4, 40) && lit(44 + 4, 40));
    TEST_ASSERT_FALSE(lit(44 - RADIUS - 3, 40) || lit(44 + RADIUS + 3, 40));
    fixture_end();
}

static void
test_on_a_diagonal_the_radius_is_measured_across_the_line(void) {
    fixture_begin();
    for (int x = 0; x < PANEL_W; x++) {
        heights[x] = (int16_t)((x - 8) * GFX_GLOW_ONE);
    }
    draw_identity(whole_panel());
    /* 10 rows above the line is 7.1 px from it; 13 rows is 9.2 px. */
    TEST_ASSERT_TRUE(lit(48, 40 - 10));
    TEST_ASSERT_FALSE(lit(48, 40 - 13));
    fixture_end();
}

static void
test_erase_rows_are_blackened_and_the_rest_left_alone(void) {
    fixture_begin();
    flat_curve(40);
    draw_identity(whole_panel());
    flat_curve(43);
    const gfx_glow_field_t field = posed_field_count(PANEL_W);
    gfx_glow_draw_posed_rows(whole_panel(), 0, 0, PANEL_W, PANEL_H, PANEL_W, PANEL_H, &field, NULL, PANEL_H,
                             (gfx_glow_pose_t){0, GFX_GLOW_POSE_ONE}, 0, PANEL_H, lit_lo, lit_hi, 0, style);
    TEST_ASSERT_EQUAL_HEX16(GFX_RGB(0x000000), pixels[(40 - RADIUS + 2) * PANEL_W + 10]);
    TEST_ASSERT_EQUAL_HEX16(UNTOUCHED, pixels[(40 + RADIUS + 6) * PANEL_W + 10]);
    TEST_ASSERT_EQUAL_HEX16(UNTOUCHED, pixels[(40 - RADIUS - 6) * PANEL_W + 10]);
    fixture_end();
}

static void
test_the_reported_box_covers_exactly_what_was_written(void) {
    fixture_begin();
    flat_curve(40);
    const gfx_glow_box_t box = draw_identity(whole_panel());
    TEST_ASSERT_EQUAL_INT(0, box.x0);
    TEST_ASSERT_EQUAL_INT(PANEL_W, box.x1);
    for (int y = 0; y < PANEL_H; y++) {
        for (int x = 0; x < PANEL_W; x++) {
            const bool inside = x >= box.x0 && x < box.x1 && y >= box.y0 && y < box.y1;
            TEST_ASSERT_TRUE(inside || pixels[y * PANEL_W + x] == UNTOUCHED);
        }
    }
    TEST_ASSERT_TRUE(pixels[40 * PANEL_W + 0] != UNTOUCHED);
    TEST_ASSERT_TRUE(pixels[40 * PANEL_W + PANEL_W - 1] != UNTOUCHED);
    fixture_end();
}

static void
test_a_quarter_turn_draws_the_same_curve_turned(void) {
    fixture_begin();
    for (int x = 0; x < PANEL_H; x++) {
        heights[x] = (int16_t)((30 + x / 4) * GFX_GLOW_ONE);
    }
    draw_curve(whole_panel(), PANEL_H, PANEL_W, (gfx_glow_pose_t){-GFX_GLOW_POSE_ONE, 0}, 0, 0, PANEL_W, PANEL_H, 0,
               PANEL_H);
    for (int view_x = 0; view_x < PANEL_H; view_x += 9) {
        const int on_curve = 30 + view_x / 4;
        TEST_ASSERT_TRUE(lit(PANEL_W - 1 - on_curve, view_x));
        TEST_ASSERT_FALSE(lit(PANEL_W - 1 - (on_curve + RADIUS + 2), view_x));
    }
    fixture_end();
}

static void
test_clip_and_a_band_target_bound_the_writes(void) {
    fixture_begin();
    flat_curve(40);
    draw_curve(whole_panel(), PANEL_W, PANEL_H, (gfx_glow_pose_t){0, GFX_GLOW_POSE_ONE}, 10, 38, 20, 42, 0, PANEL_H);
    for (int y = 0; y < PANEL_H; y++) {
        for (int x = 0; x < PANEL_W; x++) {
            const bool inside = x >= 10 && x < 20 && y >= 38 && y < 42;
            TEST_ASSERT_EQUAL_INT(inside, pixels[y * PANEL_W + x] != UNTOUCHED);
        }
    }

    /* A band whose row 0 is panel row 36: the curve's row 40 lands on local row 4. */
    for (int i = 0; i < PANEL_W * PANEL_H; i++) {
        pixels[i] = UNTOUCHED;
    }
    const gfx_target_t band = {pixels, 36, 8, PANEL_W};
    draw_identity(band);
    TEST_ASSERT_EQUAL_HEX16(GFX_RGB(0xFFFFFF), pixels[4 * PANEL_W + 5]);
    TEST_ASSERT_EQUAL_HEX16(UNTOUCHED, pixels[8 * PANEL_W + 5]);
    fixture_end();
}

/* The curve at any angle. The view frame of these tests is the panel turned
 * a quarter: PANEL_H columns, PANEL_W tall. */

static gfx_glow_field_t
posed_field(void) {
    return posed_field_count(PANEL_H);
}

static void
forget_what_was_lit(void) {
    memset(lit_lo, 0, sizeof lit_lo);
    memset(lit_hi, 0, sizeof lit_hi);
}

static void
draw_posed(const gfx_glow_field_t* field, int down_x, int down_y) {
    gfx_glow_draw_posed_rows(whole_panel(), 0, 0, PANEL_W, PANEL_H, PANEL_W, PANEL_H, field, posed_map, PANEL_W,
                             (gfx_glow_pose_t){down_x, down_y}, 0, PANEL_H, lit_lo, lit_hi, posed_trail, style);
}

static void
wavy_curve_for_the_turned_view(void) {
    for (int x = 0; x < PANEL_H; x++) {
        heights[x] = (int16_t)((40 + ((x / 9) % 2 ? 11 : -7) + x / 5) * GFX_GLOW_ONE + 5);
    }
}

static void
test_narrow_keeps_exactly_the_steps_inside_the_bounds(void) {
    const int64_t steps[] = {-16384, -11585, -3, -1, 0, 1, 7, 11585, 16384};
    for (size_t s = 0; s < sizeof steps / sizeof steps[0]; s++) {
        for (int64_t v0 = -40000; v0 <= 40000; v0 += 9973) {
            int a = 3;
            int b = 90;
            gfx_glow_narrow(v0, steps[s], -5000, 20000, &a, &b);
            for (int p = 3; p < 90; p++) {
                const int64_t v = v0 + p * steps[s];
                const bool inside = v >= -5000 && v < 20000;
                TEST_ASSERT_EQUAL_INT(inside, p >= a && p < b);
            }
        }
    }
}

/* 0.6, 0.8 and the like are exact Q14 unit vectors only approximately; what
 * matters is that the same pose draws the same picture however it got there. */
static void
test_turning_leaves_no_trail(void) {
    fixture_begin();
    wavy_curve_for_the_turned_view();
    const gfx_glow_field_t field = posed_field();
    const int poses[][2] = {{-16384, 0}, {-13107, 9830}, {0, 16384}, {11585, 11585}, {16384, 0}, {-9830, -13107}};
    gfx_color_t* fresh = malloc(sizeof(gfx_color_t) * PANEL_W * PANEL_H);
    TEST_ASSERT_NOT_NULL(fresh);

    for (int i = 0; i < PANEL_W * PANEL_H; i++) {
        pixels[i] = GFX_RGB(0x000000);
    }
    forget_what_was_lit();
    for (size_t p = 0; p < sizeof poses / sizeof poses[0]; p++) {
        draw_posed(&field, poses[p][0], poses[p][1]);

        gfx_color_t* turned = pixels;
        int16_t kept_lo[PANEL_H];
        int16_t kept_hi[PANEL_H];
        memcpy(kept_lo, lit_lo, sizeof kept_lo);
        memcpy(kept_hi, lit_hi, sizeof kept_hi);
        pixels = fresh;
        for (int i = 0; i < PANEL_W * PANEL_H; i++) {
            pixels[i] = GFX_RGB(0x000000);
        }
        forget_what_was_lit();
        draw_posed(&field, poses[p][0], poses[p][1]);
        pixels = turned;
        memcpy(lit_lo, kept_lo, sizeof kept_lo);
        memcpy(lit_hi, kept_hi, sizeof kept_hi);

        TEST_ASSERT_EQUAL_MEMORY(fresh, pixels, sizeof(gfx_color_t) * PANEL_W * PANEL_H);
    }
    free(fresh);
    fixture_end();
}

static void
test_a_turned_curve_keeps_its_width(void) {
    fixture_begin();
    for (int x = 0; x < PANEL_H; x++) {
        heights[x] = (int16_t)(48 * GFX_GLOW_ONE);
    }
    const gfx_glow_field_t field = posed_field();
    for (int i = 0; i < PANEL_W * PANEL_H; i++) {
        pixels[i] = GFX_RGB(0x000000);
    }
    forget_what_was_lit();
    /* Down at 45 degrees: the flat curve is a diagonal through the panel's
     * centre, and light reaches RADIUS across it, not RADIUS along a row. */
    draw_posed(&field, -11585, 11585);
    const int cx = PANEL_W / 2;
    const int cy = PANEL_H / 2;
    TEST_ASSERT_TRUE(lit(cx, cy));
    TEST_ASSERT_TRUE(lit(cx + (RADIUS - 2) * 7 / 10, cy - (RADIUS - 2) * 7 / 10));
    TEST_ASSERT_FALSE(lit(cx + (RADIUS + 3) * 7 / 10 + 1, cy - (RADIUS + 3) * 7 / 10 - 1));
    TEST_ASSERT_TRUE(lit(cx + 12, cy + 12));
    fixture_end();
}

static void
clear_panel_and_forget(void) {
    for (int i = 0; i < PANEL_W * PANEL_H; i++) {
        pixels[i] = GFX_RGB(0x000000);
    }
    forget_what_was_lit();
}

/* The narrowed walk pinned to an un-narrowed truth: every panel pixel,
 * evaluated directly against the per-column reach test with no narrowing at
 * all, at a wide sweep of poses. */

static void
cliff_curve_for_the_turned_view(void) {
    for (int x = 0; x < PANEL_H; x++) {
        const int row = x < 30 ? 15 : (x < 34 ? 15 + (x - 30) * 55 : 235);
        heights[x] = (int16_t)(row * GFX_GLOW_ONE);
    }
}

static void
runs_off_both_ends_curve_for_the_turned_view(void) {
    for (int x = 0; x < PANEL_H; x++) {
        heights[x] = (int16_t)((-24 + x * 3) * GFX_GLOW_ONE);
    }
}

/* Not exactly unit-length Q14 poses only approximately; what matters is the
 * same integer arithmetic the code itself would do with them. */
static gfx_glow_pose_t
normalized_pose(int32_t dx, int32_t dy) {
    const int64_t len = (int64_t)gfx_glow_isqrt((uint32_t)(dx * dx + dy * dy));
    if (len == 0) {
        return (gfx_glow_pose_t){GFX_GLOW_POSE_ONE, 0};
    }
    return (gfx_glow_pose_t){(int32_t)((int64_t)dx * GFX_GLOW_POSE_ONE / len),
                             (int32_t)((int64_t)dy * GFX_GLOW_POSE_ONE / len)};
}

static gfx_color_t
truth_pixel(const gfx_glow_field_t* field, int view_h, gfx_glow_pose_t pose, int px, int py) {
    /* The same single division gfx_glow_draw_posed_rows() does, at pixel 0
     * of the row, then pure integer steps to px - not a fresh division at
     * px, which truncates differently and would not be the same truth. */
    const int64_t right_x = pose.down_y;
    const int64_t right_y = -pose.down_x;
    const int64_t half_q14 = GFX_GLOW_POSE_ONE / 2;
    const int to_q4 = 14 - GFX_GLOW_Q_SHIFT;
    const int64_t dx2_0 = -(int64_t)(PANEL_W - 1);
    const int64_t dy2 = 2 * (int64_t)py - (PANEL_H - 1);
    const int64_t vx0 =
        ((int64_t)(field->count - 1) * GFX_GLOW_POSE_ONE + dx2_0 * right_x + dy2 * right_y) / 2 + half_q14;
    const int64_t vy0 =
        ((int64_t)(view_h - 1) * GFX_GLOW_POSE_ONE + dx2_0 * pose.down_x + dy2 * pose.down_y) / 2 + half_q14;
    const int64_t vx = vx0 + (int64_t)px * right_x;
    const int64_t vy = vy0 + (int64_t)px * pose.down_x;
    const int x_q4 = (int)(vx >> to_q4);
    const int y_q4 = (int)(vy >> to_q4);
    const int column = x_q4 >> GFX_GLOW_Q_SHIFT;
    if (column < 0 || column >= field->count || y_q4 < field->reach_lo[column] || y_q4 > field->reach_hi[column]) {
        return GFX_RGB(0x000000);
    }
    return gfx_glow_colour(style, gfx_glow_posed_distance2(field, posed_map, style->radius, x_q4, y_q4), px, py);
}

static void
assert_draw_matches_truth(const gfx_glow_field_t* field, gfx_glow_pose_t pose) {
    for (int py = 0; py < PANEL_H; py++) {
        for (int px = 0; px < PANEL_W; px++) {
            truth[py * PANEL_W + px] = truth_pixel(field, PANEL_W, pose, px, py);
        }
    }
    clear_panel_and_forget();
    draw_posed(field, pose.down_x, pose.down_y);
    TEST_ASSERT_EQUAL_MEMORY(truth, pixels, sizeof(gfx_color_t) * PANEL_W * PANEL_H);
}

static void
assert_curve_matches_truth_at_every_pose(void) {
    const gfx_glow_field_t field = posed_field();
    const int32_t axis[][2] = {
        {GFX_GLOW_POSE_ONE, 0}, {-GFX_GLOW_POSE_ONE, 0}, {0, GFX_GLOW_POSE_ONE}, {0, -GFX_GLOW_POSE_ONE}};
    for (size_t i = 0; i < sizeof axis / sizeof axis[0]; i++) {
        assert_draw_matches_truth(&field, (gfx_glow_pose_t){axis[i][0], axis[i][1]});
    }
    const int32_t diag[][2] = {{11585, 11585}, {11585, -11585}, {-11585, 11585}, {-11585, -11585}};
    for (size_t i = 0; i < sizeof diag / sizeof diag[0]; i++) {
        assert_draw_matches_truth(&field, (gfx_glow_pose_t){diag[i][0], diag[i][1]});
    }
    /* (16384, 0) nudged by one unit each way, and (3, 16383)-like near-axis
     * poses, renormalised close to a Q14 unit vector. */
    const int32_t awkward[][2] = {
        {16384, 1}, {16384, -1}, {-16384, 1}, {-16384, -1}, {1, 16384}, {-1, 16384}, {1, -16384}, {-1, -16384},
        {3, 16383}, {-3, 16383}, {3, -16383}, {-3, -16383}, {16383, 3}, {16383, -3}, {-16383, 3}, {-16383, -3},
    };
    for (size_t i = 0; i < sizeof awkward / sizeof awkward[0]; i++) {
        assert_draw_matches_truth(&field, normalized_pose(awkward[i][0], awkward[i][1]));
    }
    for (int i = 0; i < 32; i++) {
        const uint16_t phase = (uint16_t)(i * 65536u / 32);
        assert_draw_matches_truth(&field, normalized_pose(trig_cos(phase), trig_sin(phase)));
    }
}

static void
test_the_narrowed_walk_matches_every_pixel_searched_on_a_smooth_curve(void) {
    fixture_begin();
    wavy_curve_for_the_turned_view();
    assert_curve_matches_truth_at_every_pose();
    fixture_end();
}

static void
test_the_narrowed_walk_matches_every_pixel_searched_at_a_cliff(void) {
    fixture_begin();
    cliff_curve_for_the_turned_view();
    assert_curve_matches_truth_at_every_pose();
    fixture_end();
}

static void
test_the_narrowed_walk_matches_every_pixel_searched_off_both_ends_of_the_panel(void) {
    fixture_begin();
    runs_off_both_ends_curve_for_the_turned_view();
    assert_curve_matches_truth_at_every_pose();
    fixture_end();
}

static void
test_a_trail_that_is_never_cleared_keeps_where_the_curve_was(void) {
    fixture_begin();
    wavy_curve_for_the_turned_view();
    const gfx_glow_field_t field = posed_field();
    clear_panel_and_forget();
    posed_trail = 255;
    draw_posed(&field, -GFX_GLOW_POSE_ONE, 0);
    const int on_the_first_curve_x = PANEL_W - 1 - (heights[10] >> GFX_GLOW_Q_SHIFT);
    const gfx_color_t core = pixels[10 * PANEL_W + on_the_first_curve_x];
    draw_posed(&field, 0, GFX_GLOW_POSE_ONE);
    TEST_ASSERT_EQUAL_HEX16(core, pixels[10 * PANEL_W + on_the_first_curve_x]);
    posed_trail = 0;
    fixture_end();
}

static void
test_a_fading_trail_dims_then_ends_on_the_clean_picture(void) {
    fixture_begin();
    wavy_curve_for_the_turned_view();
    const gfx_glow_field_t field = posed_field();
    gfx_color_t* clean = malloc(sizeof(gfx_color_t) * PANEL_W * PANEL_H);
    TEST_ASSERT_NOT_NULL(clean);
    clear_panel_and_forget();
    draw_posed(&field, 0, GFX_GLOW_POSE_ONE);
    memcpy(clean, pixels, sizeof(gfx_color_t) * PANEL_W * PANEL_H);

    clear_panel_and_forget();
    posed_trail = 200;
    draw_posed(&field, -GFX_GLOW_POSE_ONE, 0);
    const int on_the_first_curve_x = PANEL_W - 1 - (heights[10] >> GFX_GLOW_Q_SHIFT);
    const int lit_at_first = brightness(pixels[10 * PANEL_W + on_the_first_curve_x]);

    draw_posed(&field, 0, GFX_GLOW_POSE_ONE);
    const int a_draw_later = brightness(pixels[10 * PANEL_W + on_the_first_curve_x]);
    TEST_ASSERT_TRUE(a_draw_later > 0 && a_draw_later < lit_at_first);

    for (int draw_index = 1; draw_index < gfx_glow_trail_draws(200); draw_index++) {
        draw_posed(&field, 0, GFX_GLOW_POSE_ONE);
    }
    TEST_ASSERT_EQUAL_MEMORY(clean, pixels, sizeof(gfx_color_t) * PANEL_W * PANEL_H);
    posed_trail = 0;
    free(clean);
    fixture_end();
}

static void
test_trail_draws_is_how_long_white_takes_to_go_black(void) {
    TEST_ASSERT_EQUAL_INT(0, gfx_glow_trail_draws(0));
    TEST_ASSERT_EQUAL_INT(0, gfx_glow_trail_draws(255));
    const int trails[] = {1, 32, 128, 200, 232, 254};
    for (size_t t = 0; t < sizeof trails / sizeof trails[0]; t++) {
        gfx_color_t colour = GFX_RGB(0xFFFFFF);
        int draws = 0;
        while (colour != GFX_RGB(0x000000) && draws < 10000) {
            colour = gfx_glow_dim(colour, trails[t]);
            draws++;
        }
        TEST_ASSERT_EQUAL_INT(draws, gfx_glow_trail_draws(trails[t]));
    }
}

static void
test_dimming_a_grey_keeps_it_grey_all_the_way_to_black(void) {
    gfx_color_t colour = GFX_RGB(0xFFFFFF);
    for (int i = 0; i < 100 && colour != GFX_RGB(0x000000); i++) {
        colour = gfx_glow_dim(colour, 220);
        const uint32_t rgb = gfx_color_rgb888(colour);
        const int r = (int)(rgb >> 16);
        const int g = (int)((rgb >> 8) & 0xFF);
        const int b = (int)(rgb & 0xFF);
        TEST_ASSERT_INT_WITHIN(8, r, g);
        TEST_ASSERT_EQUAL_INT(r, b);
    }
    TEST_ASSERT_EQUAL_HEX16(GFX_RGB(0x000000), colour);
}

/* 0x30C0FF sums to 495; rounding to RGB565 moves it a little by phase. */
#define FULL_HALO_AT_LEAST 470

static int
phases_lit(int i) {
    int count = 0;
    for (int phase = 0; phase < GFX_GLOW_PHASES; phase++) {
        count += style->ramp[phase][i] != GFX_RGB(0x000000);
    }
    return count;
}

/* One step of light is a stipple: past the core a pixel is the halo colour
 * at full or it is black, and fewer of them are lit the further out. */
static void
test_one_step_of_light_is_lit_or_black_and_thins_toward_the_rim(void) {
    fixture_begin();
    gfx_glow_style_set_stepped(style, RADIUS, 2, 0xFFFFFF, 0x30C0FF, 1);
    const int past_the_core = GFX_GLOW_RAMP_SIZE * 2 / RADIUS + 1;

    for (int i = past_the_core; i < GFX_GLOW_RAMP_SIZE; i++) {
        for (int phase = 0; phase < GFX_GLOW_PHASES; phase++) {
            const gfx_color_t c = style->ramp[phase][i];
            TEST_ASSERT_TRUE(c == GFX_RGB(0x000000) || brightness(c) > FULL_HALO_AT_LEAST);
        }
        TEST_ASSERT_TRUE(phases_lit(i) <= phases_lit(i - 1));
    }
    TEST_ASSERT_TRUE(phases_lit(past_the_core) > phases_lit(GFX_GLOW_RAMP_SIZE / 2));
    TEST_ASSERT_TRUE(phases_lit(GFX_GLOW_RAMP_SIZE / 2) > 0);
    TEST_ASSERT_EQUAL_INT(0, phases_lit(GFX_GLOW_RAMP_SIZE - 1));
    fixture_end();
}

static void
test_no_steps_is_the_smooth_ramp(void) {
    fixture_begin();
    gfx_glow_style_t* stepped = malloc(sizeof *stepped);
    TEST_ASSERT_NOT_NULL(stepped);
    gfx_glow_style_set_stepped(stepped, RADIUS, 2, 0xFFFFFF, 0x30C0FF, 0);
    TEST_ASSERT_EQUAL_MEMORY(style->ramp, stepped->ramp, sizeof style->ramp);
    free(stepped);
    fixture_end();
}

/* The map of the curve's light. */

#define MAP_COLS ((PANEL_H + GFX_GLOW_MAP_CELL - 1) / GFX_GLOW_MAP_CELL)
#define MAP_ROWS 64

static uint16_t* map_cells;
static int32_t map_row_f[MAP_COLS];
static int32_t map_row_z[MAP_COLS];
static int16_t map_row_v[MAP_COLS];

static gfx_glow_map_t
built_map(const gfx_glow_field_t* field) {
    gfx_glow_map_t map = {map_cells, map_row_f, map_row_z, map_row_v, MAP_COLS, MAP_ROWS, 0, 0, 0};
    gfx_glow_map_build(&map, field, style);
    return map;
}

/* How far, in the map's units, a cell's centre row is from the curve within
 * that cell's own columns: what the first pass raises a parabola from. */
static int
up_to_the_curve_in_cell(const gfx_glow_field_t* field, int cell, int centre) {
    int nearest = INT32_MAX;
    for (int j = cell * GFX_GLOW_MAP_CELL; j < (cell + 1) * GFX_GLOW_MAP_CELL && j < field->count; j++) {
        const int up =
            gfx_glow_outside(centre, field->span_lo[j], field->span_hi[j]) >> (GFX_GLOW_Q_SHIFT - GFX_GLOW_MAP_Q);
        nearest = up < nearest ? up : nearest;
    }
    return nearest;
}

/* A cell's value worked out the slow way: every column tried. */
static uint16_t
brute_force_cell(const gfx_glow_field_t* field, const gfx_glow_map_t* map, int row, int cell) {
    const int step = GFX_GLOW_MAP_CELL << GFX_GLOW_MAP_Q;
    const int reach = (RADIUS + GFX_GLOW_MAP_CELL) << GFX_GLOW_MAP_Q;
    const int centre =
        ((map->origin_y + row * GFX_GLOW_MAP_CELL) << GFX_GLOW_Q_SHIFT) + (GFX_GLOW_MAP_CELL << GFX_GLOW_Q_SHIFT) / 2;
    int64_t best = map->far;
    for (int other = 0; other < MAP_COLS; other++) {
        const int up = up_to_the_curve_in_cell(field, other, centre);
        if (up >= reach) {
            continue;
        }
        const int64_t d2 = (int64_t)step * step * (cell - other) * (cell - other) + (int64_t)up * up;
        best = d2 < best ? d2 : best;
    }
    return (uint16_t)best;
}

/* The second pass is a lower envelope worked out in one sweep. Its answer is
 * checked against trying every column, for every cell. */
static void
test_the_map_is_the_brute_force_distance_in_every_cell(void) {
    fixture_begin();
    map_cells = malloc(sizeof(uint16_t) * MAP_COLS * MAP_ROWS);
    TEST_ASSERT_NOT_NULL(map_cells);
    wavy_curve_for_the_turned_view();
    const gfx_glow_field_t field = posed_field();
    const gfx_glow_map_t map = built_map(&field);
    TEST_ASSERT_GREATER_THAN_INT(8, map.lit_rows);

    for (int row = 0; row < map.lit_rows; row++) {
        for (int c = 0; c < MAP_COLS; c++) {
            TEST_ASSERT_EQUAL_UINT16(brute_force_cell(&field, &map, row, c), map.cells[row * MAP_COLS + c]);
        }
    }
    free(map_cells);
    fixture_end();
}

static void
test_a_mapped_draw_is_the_searched_one_on_the_line_and_close_to_it_around(void) {
    fixture_begin();
    map_cells = malloc(sizeof(uint16_t) * MAP_COLS * MAP_ROWS);
    gfx_color_t* searched = malloc(sizeof(gfx_color_t) * PANEL_W * PANEL_H);
    TEST_ASSERT_NOT_NULL(map_cells);
    TEST_ASSERT_NOT_NULL(searched);
    wavy_curve_for_the_turned_view();
    const gfx_glow_field_t field = posed_field();
    const gfx_glow_map_t map = built_map(&field);
    const int poses[][2] = {{-16384, 0}, {-13107, 9830}, {0, 16384}, {11585, 11585}};

    for (size_t p = 0; p < sizeof poses / sizeof poses[0]; p++) {
        clear_panel_and_forget();
        draw_posed(&field, poses[p][0], poses[p][1]);
        memcpy(searched, pixels, sizeof(gfx_color_t) * PANEL_W * PANEL_H);

        clear_panel_and_forget();
        posed_map = &map;
        draw_posed(&field, poses[p][0], poses[p][1]);
        posed_map = NULL;

        int brightest_pixels_that_differ = 0;
        long total_difference = 0;
        long lit_pixels = 0;
        for (int i = 0; i < PANEL_W * PANEL_H; i++) {
            const int difference = abs(brightness(searched[i]) - brightness(pixels[i]));
            TEST_ASSERT_LESS_THAN_INT(80, difference);
            total_difference += difference;
            lit_pixels += searched[i] != GFX_RGB(0x000000);
            /* within the searched window the two are one code path */
            brightest_pixels_that_differ += brightness(searched[i]) > 500 && difference != 0;
        }
        TEST_ASSERT_EQUAL_INT(0, brightest_pixels_that_differ);
        TEST_ASSERT_GREATER_THAN_INT(500, (int)lit_pixels);
        TEST_ASSERT_LESS_THAN_INT(8, (int)(total_difference / lit_pixels));
    }
    free(searched);
    free(map_cells);
    fixture_end();
}

static void
test_outside_the_maps_rows_there_is_no_light(void) {
    fixture_begin();
    map_cells = malloc(sizeof(uint16_t) * MAP_COLS * MAP_ROWS);
    TEST_ASSERT_NOT_NULL(map_cells);
    for (int x = 0; x < PANEL_H; x++) {
        heights[x] = (int16_t)(48 * GFX_GLOW_ONE);
    }
    const gfx_glow_field_t field = posed_field();
    const gfx_glow_map_t map = built_map(&field);
    const int far_q8 = (int)map.far << (2 * (GFX_GLOW_Q_SHIFT - GFX_GLOW_MAP_Q));
    TEST_ASSERT_EQUAL_INT(far_q8, gfx_glow_map_distance2(&map, 20 * GFX_GLOW_ONE, 2 * GFX_GLOW_ONE));
    TEST_ASSERT_EQUAL_INT(far_q8, gfx_glow_map_distance2(&map, 20 * GFX_GLOW_ONE, 94 * GFX_GLOW_ONE));
    /* and on the curve, in the middle of the map, there is none to go */
    TEST_ASSERT_LESS_THAN_INT(2 * GFX_GLOW_ONE * 2 * GFX_GLOW_ONE,
                              gfx_glow_map_distance2(&map, 40 * GFX_GLOW_ONE, 48 * GFX_GLOW_ONE));
    /* past either end of the curve the end cells stand, rather than a read out of bounds */
    TEST_ASSERT_TRUE(gfx_glow_map_distance2(&map, -50 * GFX_GLOW_ONE, 48 * GFX_GLOW_ONE) < far_q8);
    TEST_ASSERT_TRUE(gfx_glow_map_distance2(&map, (PANEL_H + 50) * GFX_GLOW_ONE, 48 * GFX_GLOW_ONE) < far_q8);
    free(map_cells);
    fixture_end();
}

void
suite_gfx_glow(void) {
    RUN_TEST(test_ramp_index_runs_from_the_curve_to_the_rim_without_a_square_root);
    RUN_TEST(test_every_phase_fades_from_the_core_colour_to_black);
    RUN_TEST(test_phases_round_one_colour_at_different_thresholds);
    RUN_TEST(test_one_step_of_light_is_lit_or_black_and_thins_toward_the_rim);
    RUN_TEST(test_no_steps_is_the_smooth_ramp);
    RUN_TEST(test_a_flat_curve_lights_its_radius_and_no_further);
    RUN_TEST(test_a_cliff_lights_nothing_far_above_the_plateau);
    RUN_TEST(test_on_a_diagonal_the_radius_is_measured_across_the_line);
    RUN_TEST(test_erase_rows_are_blackened_and_the_rest_left_alone);
    RUN_TEST(test_the_reported_box_covers_exactly_what_was_written);
    RUN_TEST(test_a_quarter_turn_draws_the_same_curve_turned);
    RUN_TEST(test_clip_and_a_band_target_bound_the_writes);
    RUN_TEST(test_narrow_keeps_exactly_the_steps_inside_the_bounds);
    RUN_TEST(test_turning_leaves_no_trail);
    RUN_TEST(test_a_turned_curve_keeps_its_width);
    RUN_TEST(test_the_narrowed_walk_matches_every_pixel_searched_on_a_smooth_curve);
    RUN_TEST(test_the_narrowed_walk_matches_every_pixel_searched_at_a_cliff);
    RUN_TEST(test_the_narrowed_walk_matches_every_pixel_searched_off_both_ends_of_the_panel);
    RUN_TEST(test_a_trail_that_is_never_cleared_keeps_where_the_curve_was);
    RUN_TEST(test_a_fading_trail_dims_then_ends_on_the_clean_picture);
    RUN_TEST(test_trail_draws_is_how_long_white_takes_to_go_black);
    RUN_TEST(test_dimming_a_grey_keeps_it_grey_all_the_way_to_black);
    RUN_TEST(test_the_map_is_the_brute_force_distance_in_every_cell);
    RUN_TEST(test_a_mapped_draw_is_the_searched_one_on_the_line_and_close_to_it_around);
    RUN_TEST(test_outside_the_maps_rows_there_is_no_light);
}

SUITE_REGISTER(suite_gfx_glow);

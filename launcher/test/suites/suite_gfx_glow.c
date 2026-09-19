/*
 * Portable suite: gfx_glow.h - a curve drawn as light by true distance.
 * Drives gfx_glow_draw_columns() into a small buffer of its own; gfx.c's
 * wrapper adds only guards and dirty marking.
 */

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "suites.h"
#include "unity.h"

#include "gfx/gfx_glow.h"
#include "ui/ui_transform.h"

#define PANEL_W   96
#define PANEL_H   80
#define RADIUS    8
#define UNTOUCHED ((gfx_color_t)0x1234)

static gfx_color_t* pixels;
static gfx_glow_style_t* style;
static int16_t heights[PANEL_W];

static void
fixture_begin(void) {
    pixels = malloc(sizeof(gfx_color_t) * PANEL_W * PANEL_H);
    style = malloc(sizeof *style);
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
    free(style);
    pixels = NULL;
    style = NULL;
}

static gfx_target_t
whole_panel(void) {
    return (gfx_target_t){pixels, 0, PANEL_H, PANEL_W};
}

static void
draw(gfx_target_t target, int count, int chunk, int quarter_turns, int erase_px) {
    for (int x = 0; x < count; x += chunk) {
        gfx_glow_draw_columns(target, 0, 0, PANEL_W, PANEL_H, PANEL_W, PANEL_H, heights, count, x, x + chunk,
                              quarter_turns, erase_px, style);
    }
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
test_turning_into_the_panel_matches_ui_transform(void) {
    for (int quarter = 0; quarter < 4; quarter++) {
        const bool turned = quarter & 1;
        const int view_w = turned ? PANEL_H : PANEL_W;
        const int view_h = turned ? PANEL_W : PANEL_H;
        const ui_transform_t transform = ui_transform_quarter_turn(quarter, PANEL_W, PANEL_H);
        const int corners[4][2] = {{0, 0}, {view_w - 1, 0}, {0, view_h - 1}, {view_w - 1, view_h - 1}};
        for (int c = 0; c < 4; c++) {
            const mu_Rect pixel = ui_transform_rect(transform, (mu_Rect){corners[c][0], corners[c][1], 1, 1});
            int px, py;
            gfx_glow_to_panel(quarter, PANEL_W, PANEL_H, corners[c][0], corners[c][1], &px, &py);
            TEST_ASSERT_EQUAL_INT(pixel.x, px);
            TEST_ASSERT_EQUAL_INT(pixel.y, py);
        }
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
    draw(whole_panel(), PANEL_W, GFX_GLOW_CHUNK, 0, 0);
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
    draw(whole_panel(), PANEL_W, GFX_GLOW_CHUNK, 0, 0);
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
    draw(whole_panel(), PANEL_W, GFX_GLOW_CHUNK, 0, 0);
    /* 10 rows above the line is 7.1 px from it; 13 rows is 9.2 px. */
    TEST_ASSERT_TRUE(lit(48, 40 - 10));
    TEST_ASSERT_FALSE(lit(48, 40 - 13));
    fixture_end();
}

static void
test_the_picture_does_not_depend_on_how_columns_are_chunked(void) {
    fixture_begin();
    for (int x = 0; x < PANEL_W; x++) {
        heights[x] = (int16_t)((40 + ((x / 7) % 2 ? 9 : -9)) * GFX_GLOW_ONE);
    }
    draw(whole_panel(), PANEL_W, GFX_GLOW_CHUNK, 0, 2);
    gfx_color_t* whole = malloc(sizeof(gfx_color_t) * PANEL_W * PANEL_H);
    TEST_ASSERT_NOT_NULL(whole);
    memcpy(whole, pixels, sizeof(gfx_color_t) * PANEL_W * PANEL_H);

    for (int i = 0; i < PANEL_W * PANEL_H; i++) {
        pixels[i] = UNTOUCHED;
    }
    draw(whole_panel(), PANEL_W, 5, 0, 2);
    TEST_ASSERT_EQUAL_MEMORY(whole, pixels, sizeof(gfx_color_t) * PANEL_W * PANEL_H);
    free(whole);
    fixture_end();
}

static void
test_erase_rows_are_blackened_and_the_rest_left_alone(void) {
    fixture_begin();
    flat_curve(40);
    draw(whole_panel(), PANEL_W, GFX_GLOW_CHUNK, 0, 3);
    TEST_ASSERT_EQUAL_HEX16(GFX_RGB(0x000000), pixels[(40 + RADIUS + 2) * PANEL_W + 10]);
    TEST_ASSERT_EQUAL_HEX16(GFX_RGB(0x000000), pixels[(40 - RADIUS - 2) * PANEL_W + 10]);
    TEST_ASSERT_EQUAL_HEX16(UNTOUCHED, pixels[(40 + RADIUS + 6) * PANEL_W + 10]);
    TEST_ASSERT_EQUAL_HEX16(UNTOUCHED, pixels[(40 - RADIUS - 6) * PANEL_W + 10]);
    fixture_end();
}

static void
test_the_reported_box_covers_exactly_what_was_written(void) {
    fixture_begin();
    flat_curve(40);
    const gfx_glow_box_t box = gfx_glow_draw_columns(whole_panel(), 0, 0, PANEL_W, PANEL_H, PANEL_W, PANEL_H, heights,
                                                     PANEL_W, 32, 48, 0, 0, style);
    TEST_ASSERT_EQUAL_INT(32, box.x0);
    TEST_ASSERT_EQUAL_INT(48, box.x1);
    for (int y = 0; y < PANEL_H; y++) {
        for (int x = 0; x < PANEL_W; x++) {
            const bool inside = x >= box.x0 && x < box.x1 && y >= box.y0 && y < box.y1;
            TEST_ASSERT_TRUE(inside || pixels[y * PANEL_W + x] == UNTOUCHED);
        }
    }
    TEST_ASSERT_TRUE(pixels[box.y0 * PANEL_W + 32] != UNTOUCHED);
    TEST_ASSERT_TRUE(pixels[(box.y1 - 1) * PANEL_W + 47] != UNTOUCHED);
    fixture_end();
}

static void
test_a_quarter_turn_draws_the_same_curve_turned(void) {
    fixture_begin();
    for (int x = 0; x < PANEL_H; x++) {
        heights[x] = (int16_t)((30 + x / 4) * GFX_GLOW_ONE);
    }
    draw(whole_panel(), PANEL_H, GFX_GLOW_CHUNK, 1, 0);
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
    gfx_glow_draw_columns(whole_panel(), 10, 38, 20, 42, PANEL_W, PANEL_H, heights, PANEL_W, 0, GFX_GLOW_CHUNK, 0, 0,
                          style);
    for (int y = 0; y < PANEL_H; y++) {
        for (int x = 0; x < PANEL_W; x++) {
            const bool inside = x >= 10 && x < 16 && y >= 38 && y < 42;
            TEST_ASSERT_EQUAL_INT(inside, pixels[y * PANEL_W + x] != UNTOUCHED);
        }
    }

    /* A band whose row 0 is panel row 36: the curve's row 40 lands on local row 4. */
    for (int i = 0; i < PANEL_W * PANEL_H; i++) {
        pixels[i] = UNTOUCHED;
    }
    const gfx_target_t band = {pixels, 36, 8, PANEL_W};
    gfx_glow_draw_columns(band, 0, 0, PANEL_W, PANEL_H, PANEL_W, PANEL_H, heights, PANEL_W, 0, GFX_GLOW_CHUNK, 0, 0,
                          style);
    TEST_ASSERT_EQUAL_HEX16(GFX_RGB(0xFFFFFF), pixels[4 * PANEL_W + 5]);
    TEST_ASSERT_EQUAL_HEX16(UNTOUCHED, pixels[8 * PANEL_W + 5]);
    fixture_end();
}

/* The curve at any angle. The view frame of these tests is the panel turned
 * a quarter: PANEL_H columns, PANEL_W tall. */

static int16_t posed_spans[4][PANEL_H];
static int16_t lit_lo[PANEL_H];
static int16_t lit_hi[PANEL_H];
static int posed_trail;
static int posed_trailing;

static gfx_glow_field_t
posed_field(void) {
    gfx_glow_field_t field = {posed_spans[0], posed_spans[1], posed_spans[2], posed_spans[3], PANEL_H, 0, 0};
    gfx_glow_field_prepare(&field, heights, style);
    return field;
}

static void
forget_what_was_lit(void) {
    memset(lit_lo, 0, sizeof lit_lo);
    memset(lit_hi, 0, sizeof lit_hi);
}

static void
draw_posed(const gfx_glow_field_t* field, int down_x, int down_y) {
    gfx_glow_draw_posed_rows(whole_panel(), 0, 0, PANEL_W, PANEL_H, PANEL_W, PANEL_H, field, PANEL_W,
                             (gfx_glow_pose_t){down_x, down_y}, 0, PANEL_H, lit_lo, lit_hi, posed_trail,
                             &posed_trailing, style);
}

static void
wavy_curve_for_the_turned_view(void) {
    for (int x = 0; x < PANEL_H; x++) {
        heights[x] = (int16_t)((40 + ((x / 9) % 2 ? 11 : -7) + x / 5) * GFX_GLOW_ONE + 5);
    }
}

static void
blacken_untouched(gfx_color_t* image) {
    for (int i = 0; i < PANEL_W * PANEL_H; i++) {
        if (image[i] == UNTOUCHED) {
            image[i] = GFX_RGB(0x000000);
        }
    }
}

static void
test_the_landscape_pose_is_the_quarter_turn_pixel_for_pixel(void) {
    fixture_begin();
    wavy_curve_for_the_turned_view();
    draw(whole_panel(), PANEL_H, GFX_GLOW_CHUNK, 1, 0);
    gfx_color_t* by_columns = malloc(sizeof(gfx_color_t) * PANEL_W * PANEL_H);
    TEST_ASSERT_NOT_NULL(by_columns);
    memcpy(by_columns, pixels, sizeof(gfx_color_t) * PANEL_W * PANEL_H);
    blacken_untouched(by_columns);

    for (int i = 0; i < PANEL_W * PANEL_H; i++) {
        pixels[i] = UNTOUCHED;
    }
    const gfx_glow_field_t field = posed_field();
    forget_what_was_lit();
    draw_posed(&field, -GFX_GLOW_POSE_ONE, 0);
    blacken_untouched(pixels);
    TEST_ASSERT_EQUAL_MEMORY(by_columns, pixels, sizeof(gfx_color_t) * PANEL_W * PANEL_H);
    free(by_columns);
    fixture_end();
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

    posed_trailing = 0;
    draw_posed(&field, 0, GFX_GLOW_POSE_ONE);
    const int a_draw_later = brightness(pixels[10 * PANEL_W + on_the_first_curve_x]);
    TEST_ASSERT_TRUE(a_draw_later > 0 && a_draw_later < lit_at_first);
    TEST_ASSERT_GREATER_THAN_INT(0, posed_trailing);

    int draws = 0;
    while (posed_trailing > 0 && draws < 200) {
        posed_trailing = 0;
        draw_posed(&field, 0, GFX_GLOW_POSE_ONE);
        draws++;
    }
    TEST_ASSERT_LESS_THAN_INT(200, draws);
    TEST_ASSERT_EQUAL_MEMORY(clean, pixels, sizeof(gfx_color_t) * PANEL_W * PANEL_H);
    posed_trail = 0;
    free(clean);
    fixture_end();
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

void
suite_gfx_glow(void) {
    RUN_TEST(test_turning_into_the_panel_matches_ui_transform);
    RUN_TEST(test_ramp_index_runs_from_the_curve_to_the_rim_without_a_square_root);
    RUN_TEST(test_every_phase_fades_from_the_core_colour_to_black);
    RUN_TEST(test_phases_round_one_colour_at_different_thresholds);
    RUN_TEST(test_a_flat_curve_lights_its_radius_and_no_further);
    RUN_TEST(test_a_cliff_lights_nothing_far_above_the_plateau);
    RUN_TEST(test_on_a_diagonal_the_radius_is_measured_across_the_line);
    RUN_TEST(test_the_picture_does_not_depend_on_how_columns_are_chunked);
    RUN_TEST(test_erase_rows_are_blackened_and_the_rest_left_alone);
    RUN_TEST(test_the_reported_box_covers_exactly_what_was_written);
    RUN_TEST(test_a_quarter_turn_draws_the_same_curve_turned);
    RUN_TEST(test_clip_and_a_band_target_bound_the_writes);
    RUN_TEST(test_the_landscape_pose_is_the_quarter_turn_pixel_for_pixel);
    RUN_TEST(test_narrow_keeps_exactly_the_steps_inside_the_bounds);
    RUN_TEST(test_turning_leaves_no_trail);
    RUN_TEST(test_a_turned_curve_keeps_its_width);
    RUN_TEST(test_a_trail_that_is_never_cleared_keeps_where_the_curve_was);
    RUN_TEST(test_a_fading_trail_dims_then_ends_on_the_clean_picture);
    RUN_TEST(test_dimming_a_grey_keeps_it_grey_all_the_way_to_black);
}

SUITE_REGISTER(suite_gfx_glow);

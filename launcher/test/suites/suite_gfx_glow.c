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
}

SUITE_REGISTER(suite_gfx_glow);

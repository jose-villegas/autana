/*
 * Portable suite: sand_mode_swatches - each colour mode's tile shows colours
 * that mode really has, checked against the palettes it was built from.
 */

#include <stdbool.h>

#include "suites.h"
#include "unity.h"

#include "apps/sand/sand_mode_swatches.h"
#include "gfx/gfx_color.h"

#define LUT_SIZE   256
#define UI_ENTRIES 16

static gfx_color_t none_lut[LUT_SIZE];
static gfx_color_t lut256[LUT_SIZE];
static sand_mode_swatch_t swatches[3];

/* Sixteen greys for the 16-colour mode, repeated across the table the way
 * a nearest-colour table repeats them; a distinct colour per index above
 * the UI block for 256. */
static void
fixture(void) {
    for (int i = 0; i < LUT_SIZE; i++) {
        const int grey = (i % 16) * 16;
        none_lut[i] = GFX_RGB((uint32_t)(grey << 16 | grey << 8 | grey));
        lut256[i] = GFX_RGB((uint32_t)(i < UI_ENTRIES ? 0xFF00FF : (i << 16 | (255 - i) << 8 | 0x40)));
    }
    sand_mode_swatches(none_lut, lut256, LUT_SIZE, UI_ENTRIES, swatches);
}

static bool
in_table(const gfx_color_t* table, int from, uint32_t rgb) {
    for (int i = from; i < LUT_SIZE; i++) {
        if (gfx_color_rgb888(table[i]) == rgb) {
            return true;
        }
    }
    return false;
}

static void
test_sixteen_shows_each_of_its_sixteen_colours_once(void) {
    fixture();
    const sand_mode_swatch_t* s = &swatches[SAND_COLOUR_16];
    TEST_ASSERT_EQUAL_INT(16, s->cols * s->rows);
    for (int i = 0; i < 16; i++) {
        TEST_ASSERT_TRUE(in_table(none_lut, 0, s->rgb[i]));
        for (int j = i + 1; j < 16; j++) {
            TEST_ASSERT_NOT_EQUAL(s->rgb[i], s->rgb[j]);
        }
    }
}

static void
test_sixteen_runs_dark_to_light(void) {
    fixture();
    const sand_mode_swatch_t* s = &swatches[SAND_COLOUR_16];
    for (int i = 1; i < 16; i++) {
        TEST_ASSERT_GREATER_OR_EQUAL_UINT32(s->rgb[i - 1] & 0xFF, s->rgb[i] & 0xFF);
    }
}

static void
test_256_samples_only_sand_colours(void) {
    fixture();
    const sand_mode_swatch_t* s = &swatches[SAND_COLOUR_256];
    TEST_ASSERT_GREATER_THAN_INT(16, s->cols * s->rows);
    for (int i = 0; i < s->cols * s->rows; i++) {
        TEST_ASSERT_TRUE_MESSAGE(in_table(lut256, UI_ENTRIES, s->rgb[i]), "a UI colour is not a sand colour");
    }
}

static void
test_full_sweeps_hue_in_one_row(void) {
    fixture();
    const sand_mode_swatch_t* s = &swatches[SAND_COLOUR_FULL];
    TEST_ASSERT_EQUAL_INT(1, s->rows);
    TEST_ASSERT_GREATER_THAN_INT(16 + 25, s->cols * 2);
    TEST_ASSERT_EQUAL_HEX32(0xFF0000, s->rgb[0]);
    for (int i = 1; i < s->cols; i++) {
        TEST_ASSERT_NOT_EQUAL(s->rgb[i - 1], s->rgb[i]);
    }
}

void
run_sand_mode_swatches_suite(void) {
    RUN_TEST(test_sixteen_shows_each_of_its_sixteen_colours_once);
    RUN_TEST(test_sixteen_runs_dark_to_light);
    RUN_TEST(test_256_samples_only_sand_colours);
    RUN_TEST(test_full_sweeps_hue_in_one_row);
}

SUITE_REGISTER(run_sand_mode_swatches_suite);

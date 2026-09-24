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
static sand_mode_swatch_t swatches[SAND_COLOUR_MODE_COUNT];

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

/* 0xRRGGBB -> the same weighted luma sand_mode_swatches.c sorts by, kept
 * separate so this test proves the ORDER, not merely re-runs the sort. */
static int
luma_of(uint32_t rgb) {
    return (int)(((rgb >> 16) & 0xFF) * 299 + ((rgb >> 8) & 0xFF) * 587 + (rgb & 0xFF) * 114);
}

/* Every grey has R==G==B, so sorting by any one channel or by full luma
 * gives the same order - the tests above cannot tell them apart. This
 * fixture uses real colours whose blue-channel order is the OPPOSITE of
 * their luma order, so a sort-by-blue mistake shows up as a misordering. */
static void
test_sixteen_sorts_by_luma_not_by_a_single_channel(void) {
    /* Round-tripped through GFX_RGB/gfx_color_rgb888 like every other entry,
     * since the LUT itself is quantised to RGB565 - comparing against the
     * raw literals would miss by a rounding bucket. */
    const uint32_t dark = gfx_color_rgb888(GFX_RGB(0x000060));   /* near-black, strong blue, tiny luma */
    const uint32_t bright = gfx_color_rgb888(GFX_RGB(0xC08000)); /* bright orange, no blue, large luma */

    for (int i = 0; i < LUT_SIZE; i++) {
        none_lut[i] = GFX_RGB((uint32_t)(0x101010u * (uint32_t)(i % 14) + 0x40));
    }
    none_lut[14] = GFX_RGB(0x000060);
    none_lut[15] = GFX_RGB(0xC08000);
    sand_mode_swatches(none_lut, lut256, LUT_SIZE, UI_ENTRIES, swatches);

    const sand_mode_swatch_t* s = &swatches[SAND_COLOUR_16];
    int dark_pos = -1, bright_pos = -1;
    for (int i = 0; i < 16; i++) {
        if (s->rgb[i] == dark) {
            dark_pos = i;
        }
        if (s->rgb[i] == bright) {
            bright_pos = i;
        }
    }
    TEST_ASSERT_NOT_EQUAL_MESSAGE(-1, dark_pos, "the near-black colour must appear once");
    TEST_ASSERT_NOT_EQUAL_MESSAGE(-1, bright_pos, "the bright orange colour must appear once");
    TEST_ASSERT_TRUE_MESSAGE(luma_of(dark) < luma_of(bright), "fixture sanity: the near-black is the dimmer one");
    TEST_ASSERT_LESS_THAN_INT_MESSAGE(bright_pos, dark_pos,
                                      "the dimmer colour must sort before the brighter one, even though it carries "
                                      "the larger blue channel - a sort by blue alone would misorder these two");
}

/* Fewer than sixteen distinct colours must still fill all sixteen slots, by
 * repeating the last (brightest) one - the loop after sort_by_luma() in
 * build_sixteen(). */
static void
test_sixteen_pads_by_repeating_the_last_colour_when_the_lut_has_fewer(void) {
    const int distinct = 5;
    for (int i = 0; i < LUT_SIZE; i++) {
        const int grey = (i % distinct) * 40;
        none_lut[i] = GFX_RGB((uint32_t)(grey << 16 | grey << 8 | grey));
    }
    sand_mode_swatches(none_lut, lut256, LUT_SIZE, UI_ENTRIES, swatches);

    const sand_mode_swatch_t* s = &swatches[SAND_COLOUR_16];
    for (int i = distinct; i < 16; i++) {
        TEST_ASSERT_EQUAL_HEX32_MESSAGE(s->rgb[distinct - 1], s->rgb[i],
                                        "past the last distinct colour, every slot must repeat it");
    }
}

/* The 256-sample must actually SPAN the sand range end to end, not merely
 * skip the UI block at its front - a bug that samples only the first half
 * would still pass test_256_samples_only_sand_colours above. */
static void
test_256_sample_spans_the_whole_sand_range(void) {
    fixture();
    const sand_mode_swatch_t* s = &swatches[SAND_COLOUR_256];
    const int n = s->cols * s->rows;
    const int sand_range = LUT_SIZE - UI_ENTRIES;

    int first_idx = -1, last_idx = -1;
    for (int i = UI_ENTRIES; i < LUT_SIZE; i++) {
        if (gfx_color_rgb888(lut256[i]) == s->rgb[0]) {
            first_idx = i;
        }
        if (gfx_color_rgb888(lut256[i]) == s->rgb[n - 1]) {
            last_idx = i;
        }
    }
    TEST_ASSERT_NOT_EQUAL_MESSAGE(-1, first_idx, "the first sample must be a real sand colour");
    TEST_ASSERT_NOT_EQUAL_MESSAGE(-1, last_idx, "the last sample must be a real sand colour");
    TEST_ASSERT_LESS_OR_EQUAL_INT_MESSAGE(UI_ENTRIES + sand_range / 10, first_idx,
                                          "the first sample must sit near the start of the sand range");
    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(LUT_SIZE - 1 - sand_range / 10, last_idx,
                                             "the last sample must sit near the end of the sand range, not just "
                                             "past the UI block");
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
    RUN_TEST(test_sixteen_sorts_by_luma_not_by_a_single_channel);
    RUN_TEST(test_sixteen_pads_by_repeating_the_last_colour_when_the_lut_has_fewer);
    RUN_TEST(test_256_samples_only_sand_colours);
    RUN_TEST(test_256_sample_spans_the_whole_sand_range);
    RUN_TEST(test_full_sweeps_hue_in_one_row);
}

SUITE_REGISTER(run_sand_mode_swatches_suite);

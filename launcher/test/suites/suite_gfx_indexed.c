/*
 * Portable suite: gfx_indexed - expanding a row of palette-index bytes
 * into panel pixels (gfx.h's GFX_PIXFMT_INDEXED8). Header-only and
 * ESP-IDF-free, like gfx_dirty.h's own suite - gfx.c's real allocation and
 * present-task wiring around this needs a device and is not covered here.
 */

#include <string.h>

#include "suites.h"
#include "unity.h"

#include "gfx/gfx_indexed.h"

static gfx_color_t lut[GFX_INDEXED_PALETTE_SIZE];

static void
fixture(void) {
    for (int i = 0; i < GFX_INDEXED_PALETTE_SIZE; i++) {
        lut[i] = (gfx_color_t)(0x1000 + i);
    }
}

static void
test_every_output_pixel_reads_its_own_cells_lut_entry(void) {
    fixture();
    const uint8_t row[4] = {5, 9, 200, 1};
    gfx_color_t out[16];

    gfx_indexed_expand_row(row, 4, lut, 4, out, 16);

    for (int gx = 0; gx < 4; gx++) {
        for (int dx = 0; dx < 4; dx++) {
            TEST_ASSERT_EQUAL_HEX16(lut[row[gx]], out[gx * 4 + dx]);
        }
    }
}

/* Every cell size a quality menu might request (2/3/4/6/8 px) upscales
 * correctly, not only a power-of-two one. */
static void
test_cell_size_upscale_correct_for_every_grid_quality(void) {
    fixture();
    static const int cell_sizes[] = {2, 3, 4, 6, 8};
    const uint8_t row[3] = {10, 20, 30};

    for (size_t s = 0; s < sizeof cell_sizes / sizeof cell_sizes[0]; s++) {
        const int cell = cell_sizes[s];
        gfx_color_t out[3 * 8];
        gfx_indexed_expand_row(row, 3, lut, cell, out, 3 * cell);
        for (int gx = 0; gx < 3; gx++) {
            for (int dx = 0; dx < cell; dx++) {
                TEST_ASSERT_EQUAL_HEX16_MESSAGE(lut[row[gx]], out[gx * cell + dx], "cell size mismatch");
            }
        }
    }
}

/* A cell size that does not divide the panel's own width leaves a margin -
 * app_sand.c's own top comment documents the identical margin for the
 * RGB565 path. The margin reads as lut[0], the reserved background entry. */
static void
test_the_margin_past_the_grids_own_width_is_background(void) {
    fixture();
    const uint8_t row[2] = {7, 8};
    gfx_color_t out[10];

    gfx_indexed_expand_row(row, 2, lut, 4, out, 10);

    for (int x = 0; x < 8; x++) {
        TEST_ASSERT_EQUAL_HEX16(lut[row[x / 4]], out[x]);
    }
    TEST_ASSERT_EQUAL_HEX16(lut[0], out[8]);
    TEST_ASSERT_EQUAL_HEX16(lut[0], out[9]);
}

/* A NULL grid row - a panel row past the grid's own height - reads all
 * background, the margin on the other axis. */
static void
test_a_null_grid_row_reads_all_background(void) {
    fixture();
    gfx_color_t out[8];

    gfx_indexed_expand_row(NULL, 2, lut, 4, out, 8);

    for (int x = 0; x < 8; x++) {
        TEST_ASSERT_EQUAL_HEX16(lut[0], out[x]);
    }
}

static void
test_panel_row_to_grid_row_floors_and_back_is_the_bands_first_row(void) {
    TEST_ASSERT_EQUAL_INT(0, gfx_indexed_panel_row_to_grid_row(0, 3));
    TEST_ASSERT_EQUAL_INT(0, gfx_indexed_panel_row_to_grid_row(2, 3));
    TEST_ASSERT_EQUAL_INT(1, gfx_indexed_panel_row_to_grid_row(3, 3));
    TEST_ASSERT_EQUAL_INT(3, gfx_indexed_panel_row_to_grid_row(11, 3));

    TEST_ASSERT_EQUAL_INT(0, gfx_indexed_grid_row_to_panel_row(0, 3));
    TEST_ASSERT_EQUAL_INT(9, gfx_indexed_grid_row_to_panel_row(3, 3));
}

static gfx_color_t dither_table[GFX_INDEXED_PALETTE_SIZE * GFX_INDEXED_DITHER16_PHASES];

static void
set_dither_entry(int index, int phase, gfx_color_t rgb) {
    dither_table[index * GFX_INDEXED_DITHER16_PHASES + phase] = rgb;
}

/* Every output pixel reads its own (index, phase) slot - `phase` keyed by
 * the panel coordinates the RGB565 table was baked to, `(y & 3) * 4 +
 * (x & 3)`, not a position local to this call. */
static void
test_dither_every_output_pixel_reads_its_own_index_phase_entry(void) {
    memset(dither_table, 0, sizeof dither_table);
    for (int p = 0; p < GFX_INDEXED_DITHER16_PHASES; p++) {
        set_dither_entry(7, p, (gfx_color_t)(0x1000 + p));
    }
    const uint8_t row[1] = {7};
    gfx_color_t out[4];

    for (int y = 0; y < 8; y++) {
        gfx_indexed_expand_row_dither16(row, 1, dither_table, 4, y, 0, out, 4);
        for (int x = 0; x < 4; x++) {
            const int phase = (y & 3) * 4 + (x & 3);
            TEST_ASSERT_EQUAL_HEX16((gfx_color_t)(0x1000 + phase), out[x]);
        }
    }
}

/* Same inputs, same absolute panel coordinates: two calls agree pixel for
 * pixel - the expansion has no hidden state to drift between them. */
static void
test_dither_expansion_is_deterministic_at_the_same_panel_coordinates(void) {
    memset(dither_table, 0, sizeof dither_table);
    for (int p = 0; p < GFX_INDEXED_DITHER16_PHASES; p++) {
        set_dither_entry(40, p, (gfx_color_t)(0x2000 + p));
    }
    const uint8_t row[6] = {40, 40, 40, 40, 40, 40};
    gfx_color_t a[24], b[24];

    for (int y = 0; y < 4; y++) {
        gfx_indexed_expand_row_dither16(row, 6, dither_table, 4, 100 + y, 5, a + y * 6, 6);
        gfx_indexed_expand_row_dither16(row, 6, dither_table, 4, 100 + y, 5, b + y * 6, 6);
    }
    TEST_ASSERT_EQUAL_HEX16_ARRAY(a, b, 24);
}

/* Two dithered bands sent side by side must stay in the same phase -
 * expanding one wide row in one call must equal expanding it as two
 * adjacent halves, column offset carried through panel_col0. */
static void
test_dither_expansion_stays_in_phase_across_a_band_boundary(void) {
    memset(dither_table, 0, sizeof dither_table);
    for (int p = 0; p < GFX_INDEXED_DITHER16_PHASES; p++) {
        set_dither_entry(99, p, (gfx_color_t)(0x3000 + p));
    }
    const uint8_t row[8] = {99, 99, 99, 99, 99, 99, 99, 99};

    gfx_color_t whole[32];
    gfx_indexed_expand_row_dither16(row, 8, dither_table, 4, 7, 0, whole, 32);

    gfx_color_t left[16], right[16];
    gfx_indexed_expand_row_dither16(row, 8, dither_table, 4, 7, 0, left, 16);
    gfx_indexed_expand_row_dither16(row, 8, dither_table, 4, 7, 16, right, 16);

    TEST_ASSERT_EQUAL_HEX16_ARRAY(whole, left, 16);
    TEST_ASSERT_EQUAL_HEX16_ARRAY(whole + 16, right, 16);
}

void
run_gfx_indexed_suite(void) {
    RUN_TEST(test_every_output_pixel_reads_its_own_cells_lut_entry);
    RUN_TEST(test_cell_size_upscale_correct_for_every_grid_quality);
    RUN_TEST(test_the_margin_past_the_grids_own_width_is_background);
    RUN_TEST(test_a_null_grid_row_reads_all_background);
    RUN_TEST(test_panel_row_to_grid_row_floors_and_back_is_the_bands_first_row);
    RUN_TEST(test_dither_every_output_pixel_reads_its_own_index_phase_entry);
    RUN_TEST(test_dither_expansion_is_deterministic_at_the_same_panel_coordinates);
    RUN_TEST(test_dither_expansion_stays_in_phase_across_a_band_boundary);
}

SUITE_REGISTER(run_gfx_indexed_suite);

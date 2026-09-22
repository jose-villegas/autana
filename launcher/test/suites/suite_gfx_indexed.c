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

/* A cell size that does not divide the panel's width leaves a margin, which
 * reads as lut[0], the reserved background entry. */
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

static uint8_t class_out[GFX_INDEXED_PALETTE_SIZE];

/* Two indices whose sixteen-entry rows are byte-identical land in the same
 * class - the property a caller's change detection depends on. */
static void
test_classify_groups_indices_with_an_identical_dither_row(void) {
    memset(dither_table, 0, sizeof dither_table);
    for (int p = 0; p < GFX_INDEXED_DITHER16_PHASES; p++) {
        set_dither_entry(3, p, (gfx_color_t)(0x4000 + p));
        set_dither_entry(9, p, (gfx_color_t)(0x4000 + p)); /* same row as 3 */
        set_dither_entry(5, p, (gfx_color_t)(0x5000 + p)); /* its own row */
    }

    gfx_indexed_dither16_classify(dither_table, class_out);

    TEST_ASSERT_EQUAL_UINT8(class_out[3], class_out[9]);
    TEST_ASSERT_NOT_EQUAL_UINT8(class_out[3], class_out[5]);
}

/* No two rows agree anywhere in this table: every index is its own class,
 * matching a raw index compare exactly - classifying never merges what a
 * plain equality check would have told apart. */
static void
test_classify_gives_every_index_its_own_class_when_all_rows_differ(void) {
    for (int i = 0; i < GFX_INDEXED_PALETTE_SIZE; i++) {
        for (int p = 0; p < GFX_INDEXED_DITHER16_PHASES; p++) {
            set_dither_entry(i, p, (gfx_color_t)(i * GFX_INDEXED_DITHER16_PHASES + p));
        }
    }

    gfx_indexed_dither16_classify(dither_table, class_out);

    for (int i = 0; i < GFX_INDEXED_PALETTE_SIZE; i++) {
        TEST_ASSERT_EQUAL_UINT8_MESSAGE((uint8_t)i, class_out[i], "an all-distinct table merged two indices");
    }
}

/* A class id is always the SMALLEST index sharing that row. Change detection
 * only needs equality; a stable, low id keeps a dump of the table readable. */
static void
test_classify_names_a_class_after_its_smallest_member(void) {
    memset(dither_table, 0, sizeof dither_table);
    for (int p = 0; p < GFX_INDEXED_DITHER16_PHASES; p++) {
        set_dither_entry(50, p, (gfx_color_t)(0x6000 + p));
        set_dither_entry(20, p, (gfx_color_t)(0x6000 + p));
        set_dither_entry(80, p, (gfx_color_t)(0x6000 + p));
    }

    gfx_indexed_dither16_classify(dither_table, class_out);

    TEST_ASSERT_EQUAL_UINT8(20, class_out[20]);
    TEST_ASSERT_EQUAL_UINT8(20, class_out[50]);
    TEST_ASSERT_EQUAL_UINT8(20, class_out[80]);
}

/*
 * gfx_indexed_cell_changed(): incremental output vs a full re-expansion,
 * over many steps of a busy scene. The decision is portable even where the
 * row painter calling it is not.
 */

/* Deterministic across platforms and libc versions, unlike rand() - "many
 * steps, seeds varied" must reproduce exactly on a re-run. */
static uint32_t
xorshift32(uint32_t* state) {
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

#define IC_GRID_W    6
#define IC_GRID_H    4
#define IC_CELL_SIZE 4
#define IC_STEPS     200

/* One seeded run: `incremental` only updates a cell
 * gfx_indexed_cell_changed() says changed, mirroring paint_row_n()'s own
 * rule; `truth` always takes the fresh value, standing in for a full
 * repaint every step.
 * Every step, both re-expand through the SAME table and must match pixel
 * for pixel - the dither depends only on panel position and index, so an
 * unchanged 16-colour value really does mean identical pixels. */
static void
run_incremental_matches_full_reexpansion(uint32_t seed, bool dither16_on) {
    static gfx_color_t table[GFX_INDEXED_PALETTE_SIZE * GFX_INDEXED_DITHER16_PHASES];
    /* Every 4 consecutive indices share a row - a handful of classes, not
     * 256 distinct ones, the realistic case where suppression has
     * something to catch. */
    for (int i = 0; i < GFX_INDEXED_PALETTE_SIZE; i++) {
        for (int p = 0; p < GFX_INDEXED_DITHER16_PHASES; p++) {
            table[i * GFX_INDEXED_DITHER16_PHASES + p] = (gfx_color_t)((i / 4) * 100 + p);
        }
    }
    static uint8_t class_table[GFX_INDEXED_PALETTE_SIZE];
    gfx_indexed_dither16_classify(table, class_table);

    static uint8_t incremental[IC_GRID_W * IC_GRID_H];
    static uint8_t truth[IC_GRID_W * IC_GRID_H];
    memset(incremental, 0, sizeof incremental);
    memset(truth, 0, sizeof truth);

    uint32_t rng = seed;
    for (int step = 0; step < IC_STEPS; step++) {
        for (int c = 0; c < IC_GRID_W * IC_GRID_H; c++) {
            const uint8_t new_idx = (uint8_t)(xorshift32(&rng) % GFX_INDEXED_PALETTE_SIZE);
            truth[c] = new_idx;
            if (gfx_indexed_cell_changed(incremental[c], new_idx, dither16_on, class_table)) {
                incremental[c] = new_idx;
            }
        }

        for (int cy = 0; cy < IC_GRID_H; cy++) {
            for (int dy = 0; dy < IC_CELL_SIZE; dy++) {
                const int panel_row = cy * IC_CELL_SIZE + dy;
                gfx_color_t out_incremental[IC_GRID_W * IC_CELL_SIZE];
                gfx_color_t out_truth[IC_GRID_W * IC_CELL_SIZE];
                gfx_indexed_expand_row_dither16(&incremental[cy * IC_GRID_W], IC_GRID_W, table, IC_CELL_SIZE, panel_row,
                                                0, out_incremental, IC_GRID_W * IC_CELL_SIZE);
                gfx_indexed_expand_row_dither16(&truth[cy * IC_GRID_W], IC_GRID_W, table, IC_CELL_SIZE, panel_row, 0,
                                                out_truth, IC_GRID_W * IC_CELL_SIZE);
                TEST_ASSERT_EQUAL_HEX16_ARRAY_MESSAGE(out_truth, out_incremental, IC_GRID_W * IC_CELL_SIZE,
                                                      "incremental output diverged from a full re-expansion");
            }
        }
    }
}

static void
test_incremental_16_colour_output_matches_a_full_reexpansion(void) {
    static const uint32_t seeds[] = {1, 12345, 0xDEADBEEFu, 7, 999983};
    for (size_t s = 0; s < sizeof seeds / sizeof seeds[0]; s++) {
        run_incremental_matches_full_reexpansion(seeds[s], true);
    }
}

/* Same proof at the 256-index level: dither16_on false means
 * gfx_indexed_cell_changed() falls back to a raw index compare, so every
 * distinct random index is its own "class" and nothing is ever suppressed
 * that a full repaint would have shown differently. */
static void
test_incremental_256_index_output_matches_a_full_reexpansion(void) {
    static const uint32_t seeds[] = {2, 54321, 0xC0FFEEu};
    for (size_t s = 0; s < sizeof seeds / sizeof seeds[0]; s++) {
        run_incremental_matches_full_reexpansion(seeds[s], false);
    }
}

/* The forced-repaint bypass: indices unchanged (even the same class) must
 * still come back "needs repaint" once force_full is set - a mode switch,
 * an overlay closing, an invalidate() can leave the index image already
 * holding the value about to be recomputed while the panel shows something
 * else, and narrowing on index equality then would resend nothing. */
static void
test_needs_repaint_ignores_unchanged_index_when_forced(void) {
    static uint8_t identity_class[GFX_INDEXED_PALETTE_SIZE];
    for (int i = 0; i < GFX_INDEXED_PALETTE_SIZE; i++) {
        identity_class[i] = (uint8_t)i;
    }

    TEST_ASSERT_FALSE(gfx_indexed_cell_needs_repaint(false, 5, 5, true, identity_class));
    TEST_ASSERT_TRUE(gfx_indexed_cell_needs_repaint(true, 5, 5, true, identity_class));
    TEST_ASSERT_TRUE(gfx_indexed_cell_needs_repaint(true, 5, 5, false, identity_class));
}

/* Unforced, the combinator is exactly gfx_indexed_cell_changed() - the
 * bypass only ever widens what gets repainted, never narrows it further. */
static void
test_needs_repaint_matches_cell_changed_when_not_forced(void) {
    memset(dither_table, 0, sizeof dither_table);
    for (int p = 0; p < GFX_INDEXED_DITHER16_PHASES; p++) {
        set_dither_entry(11, p, (gfx_color_t)(0x7000 + p));
        set_dither_entry(22, p, (gfx_color_t)(0x7000 + p)); /* same row as 11 */
    }
    gfx_indexed_dither16_classify(dither_table, class_out);

    static const uint8_t pairs[][2] = {{11, 22}, {11, 5}, {5, 5}};
    for (size_t i = 0; i < sizeof pairs / sizeof pairs[0]; i++) {
        for (int dither16_on = 0; dither16_on <= 1; dither16_on++) {
            const bool expected = gfx_indexed_cell_changed(pairs[i][0], pairs[i][1], dither16_on, class_out);
            const bool actual = gfx_indexed_cell_needs_repaint(false, pairs[i][0], pairs[i][1], dither16_on, class_out);
            TEST_ASSERT_EQUAL_INT_MESSAGE(expected, actual, "unforced needs_repaint diverged from cell_changed");
        }
    }
}

/* lever 2: cell dither modes */

static gfx_color_t checker_table[GFX_INDEXED_PALETTE_SIZE * GFX_INDEXED_CELL_CHECKER_PHASES];
static gfx_color_t bayer2_table[GFX_INDEXED_PALETTE_SIZE * GFX_INDEXED_CELL_BAYER2_PHASES];

/* Every cell reads its own (gx + cy) & 1 phase, whole cells solid - not a
 * per-pixel dither. */
static void
test_cell_checker_phase_is_gx_plus_cy_parity(void) {
    memset(checker_table, 0, sizeof checker_table);
    checker_table[7 * GFX_INDEXED_CELL_CHECKER_PHASES + 0] = (gfx_color_t)0xAAAA;
    checker_table[7 * GFX_INDEXED_CELL_CHECKER_PHASES + 1] = (gfx_color_t)0xBBBB;
    const uint8_t row_even[2] = {7, 7}; /* gx 0, 1 at grid row cy */

    gfx_color_t out[8];
    /* cy=0: gx=0 -> phase 0, gx=1 -> phase 1. cy=1: parities flip. */
    gfx_indexed_expand_row_dither_cell(row_even, 2, checker_table, false, 4, 0, out, 8);
    for (int x = 0; x < 4; x++) {
        TEST_ASSERT_EQUAL_HEX16(0xAAAA, out[x]);
    }
    for (int x = 4; x < 8; x++) {
        TEST_ASSERT_EQUAL_HEX16(0xBBBB, out[x]);
    }
    gfx_indexed_expand_row_dither_cell(row_even, 2, checker_table, false, 4, 1, out, 8);
    for (int x = 0; x < 4; x++) {
        TEST_ASSERT_EQUAL_HEX16(0xBBBB, out[x]);
    }
    for (int x = 4; x < 8; x++) {
        TEST_ASSERT_EQUAL_HEX16(0xAAAA, out[x]);
    }
}

/* Every cell reads its own (cy & 1) * 2 + (gx & 1) phase - the four
 * positions of a 2x2 Bayer block over cells, not pixels. */
static void
test_cell_bayer2_phase_is_2x2_cell_position(void) {
    memset(bayer2_table, 0, sizeof bayer2_table);
    for (int p = 0; p < GFX_INDEXED_CELL_BAYER2_PHASES; p++) {
        bayer2_table[3 * GFX_INDEXED_CELL_BAYER2_PHASES + p] = (gfx_color_t)(0xC000 + p);
    }
    const uint8_t row[2] = {3, 3};

    for (int cy = 0; cy < 2; cy++) {
        gfx_color_t out[8];
        gfx_indexed_expand_row_dither_cell(row, 2, bayer2_table, true, 4, cy, out, 8);
        const int phase_gx0 = cy * 2 + 0, phase_gx1 = cy * 2 + 1;
        for (int x = 0; x < 4; x++) {
            TEST_ASSERT_EQUAL_HEX16((gfx_color_t)(0xC000 + phase_gx0), out[x]);
        }
        for (int x = 4; x < 8; x++) {
            TEST_ASSERT_EQUAL_HEX16((gfx_color_t)(0xC000 + phase_gx1), out[x]);
        }
    }
}

/* A NULL grid row (a panel row past the grid's own height) reads phase 0
 * of index 0, the reserved background entry - the margin every other
 * expand function in this header shares. */
static void
test_cell_null_row_reads_background_phase(void) {
    memset(checker_table, 0, sizeof checker_table);
    checker_table[0] = (gfx_color_t)0xD00D; /* index 0, phase (gx=0)+(cy=0) & 1 == 0 */
    gfx_color_t out[4];
    gfx_indexed_expand_row_dither_cell(NULL, 1, checker_table, false, 4, 0, out, 4);
    for (int x = 0; x < 4; x++) {
        TEST_ASSERT_EQUAL_HEX16(0xD00D, out[x]);
    }
}

/* Exact, not merely safe: two indices sharing THIS cell's own phase are
 * unchanged even if they differ at some other phase this cell never
 * visits - the property that makes the cell-mode rule tighter than
 * gfx_indexed_dither16_classify()'s every-phase match. */
static void
set_agree_at_phase_0_only(void) {
    memset(bayer2_table, 0, sizeof bayer2_table);
    bayer2_table[1 * GFX_INDEXED_CELL_BAYER2_PHASES + 0] = (gfx_color_t)0x1111;
    bayer2_table[2 * GFX_INDEXED_CELL_BAYER2_PHASES + 0] = (gfx_color_t)0x1111;
    bayer2_table[1 * GFX_INDEXED_CELL_BAYER2_PHASES + 1] = (gfx_color_t)0x2222;
    bayer2_table[2 * GFX_INDEXED_CELL_BAYER2_PHASES + 1] = (gfx_color_t)0x3333;
}

static void
test_cell_dither_changed_is_exact_per_cell_not_every_phase(void) {
    set_agree_at_phase_0_only();

    TEST_ASSERT_FALSE(gfx_indexed_cell_dither_changed(1, 2, bayer2_table, true, 0, 0)); /* (0,0): phase 0 */
    TEST_ASSERT_TRUE(gfx_indexed_cell_dither_changed(1, 2, bayer2_table, true, 1, 0));  /* (1,0): phase 1 */
    TEST_ASSERT_FALSE(gfx_indexed_cell_dither_changed(1, 1, bayer2_table, true, 1, 0));
}

/* lever 1/2 unified dispatch: gfx_indexed_cell_repaint() */

/* force_full widens past every kind, even RAW with the tables left NULL -
 * the one case a stale index image must still repaint. */
static void
test_repaint_force_full_widens_every_kind(void) {
    TEST_ASSERT_TRUE(gfx_indexed_cell_repaint(GFX_INDEXED_REPAINT_RAW, NULL, NULL, true, 5, 5, 0, 0));
    TEST_ASSERT_TRUE(gfx_indexed_cell_repaint(GFX_INDEXED_REPAINT_CELL_BAYER2, NULL, bayer2_table, true, 5, 5, 0, 0));
}

/* An unmoved index answers false before any table is even read - NULL
 * tables prove no lookup happened. */
static void
test_repaint_unforced_unmoved_index_is_cheap_and_false(void) {
    TEST_ASSERT_FALSE(gfx_indexed_cell_repaint(GFX_INDEXED_REPAINT_CLASS, NULL, NULL, false, 9, 9, 0, 0));
    TEST_ASSERT_FALSE(gfx_indexed_cell_repaint(GFX_INDEXED_REPAINT_CELL_CHECKER, NULL, NULL, false, 9, 9, 3, 4));
}

/* RAW: any two distinct indices always repaint - 256 mode's own rule. */
static void
test_repaint_raw_kind_is_index_inequality(void) {
    TEST_ASSERT_TRUE(gfx_indexed_cell_repaint(GFX_INDEXED_REPAINT_RAW, NULL, NULL, false, 1, 2, 0, 0));
}

/* CLASS matches gfx_indexed_cell_changed()'s own class-table compare. */
static void
test_repaint_class_kind_matches_cell_changed(void) {
    memset(dither_table, 0, sizeof dither_table);
    for (int p = 0; p < GFX_INDEXED_DITHER16_PHASES; p++) {
        set_dither_entry(11, p, (gfx_color_t)(0x8000 + p));
        set_dither_entry(22, p, (gfx_color_t)(0x8000 + p));
    }
    gfx_indexed_dither16_classify(dither_table, class_out);

    static const uint8_t pairs[][2] = {{11, 22}, {11, 5}};
    for (size_t i = 0; i < sizeof pairs / sizeof pairs[0]; i++) {
        const bool expected = gfx_indexed_cell_changed(pairs[i][0], pairs[i][1], true, class_out);
        const bool actual =
            gfx_indexed_cell_repaint(GFX_INDEXED_REPAINT_CLASS, class_out, NULL, false, pairs[i][0], pairs[i][1], 0, 0);
        TEST_ASSERT_EQUAL_INT(expected, actual);
    }
}

/* CELL_CHECKER/CELL_BAYER2 match gfx_indexed_cell_dither_changed()'s own
 * exact per-cell phase compare, at more than one (cx, cy). */
static void
test_repaint_cell_kinds_match_cell_dither_changed(void) {
    set_agree_at_phase_0_only();

    TEST_ASSERT_FALSE(gfx_indexed_cell_repaint(GFX_INDEXED_REPAINT_CELL_BAYER2, NULL, bayer2_table, false, 1, 2, 0, 0));
    TEST_ASSERT_TRUE(gfx_indexed_cell_repaint(GFX_INDEXED_REPAINT_CELL_BAYER2, NULL, bayer2_table, false, 1, 2, 1, 0));

    memset(checker_table, 0, sizeof checker_table);
    checker_table[7 * GFX_INDEXED_CELL_CHECKER_PHASES + 0] = (gfx_color_t)0xAAAA;
    checker_table[7 * GFX_INDEXED_CELL_CHECKER_PHASES + 1] = (gfx_color_t)0xBBBB;
    checker_table[9 * GFX_INDEXED_CELL_CHECKER_PHASES + 0] = (gfx_color_t)0xAAAA;
    checker_table[9 * GFX_INDEXED_CELL_CHECKER_PHASES + 1] = (gfx_color_t)0xCCCC;
    TEST_ASSERT_FALSE(
        gfx_indexed_cell_repaint(GFX_INDEXED_REPAINT_CELL_CHECKER, NULL, checker_table, false, 7, 9, 0, 0));
    TEST_ASSERT_TRUE(
        gfx_indexed_cell_repaint(GFX_INDEXED_REPAINT_CELL_CHECKER, NULL, checker_table, false, 7, 9, 1, 0));
}

/* lever 2: GFX_DITHER_PIXEL_CHECKER2 */

static gfx_color_t
    checker2_table[GFX_INDEXED_PALETTE_SIZE * GFX_INDEXED_CHECKER2_ROW_PHASES * GFX_INDEXED_CHECKER2_CHUNK_PX];

static void
set_checker2_entry(int index, int row_phase, int chunk_px, gfx_color_t rgb) {
    checker2_table[(index * GFX_INDEXED_CHECKER2_ROW_PHASES + row_phase) * GFX_INDEXED_CHECKER2_CHUNK_PX + chunk_px] =
        rgb;
}

/* Every output pixel reads its own (index, row phase, column phase) slot -
 * phase keyed by absolute panel coordinates, a 2-pixel period instead of
 * dither16's 4x4. */
static void
test_checker2_every_output_pixel_reads_its_own_phase_entry(void) {
    memset(checker2_table, 0, sizeof checker2_table);
    for (int py = 0; py < GFX_INDEXED_CHECKER2_ROW_PHASES; py++) {
        for (int px = 0; px < GFX_INDEXED_CHECKER2_CHUNK_PX; px++) {
            set_checker2_entry(9, py, px, (gfx_color_t)(0x8000 + py * 2 + px));
        }
    }
    const uint8_t row[1] = {9};
    gfx_color_t out[4];

    for (int y = 0; y < 4; y++) {
        gfx_indexed_expand_row_dither_checker2(row, 1, checker2_table, 4, y, 0, out, 4);
        for (int x = 0; x < 4; x++) {
            const int expected_phase = (y & 1) * 2 + (x & 1);
            TEST_ASSERT_EQUAL_HEX16((gfx_color_t)(0x8000 + expected_phase), out[x]);
        }
    }
}

/* Two dithered bands sent side by side stay in phase, column offset
 * carried through panel_col0 - odd alignments included. */
static void
test_checker2_stays_in_phase_across_a_band_boundary(void) {
    memset(checker2_table, 0, sizeof checker2_table);
    for (int py = 0; py < GFX_INDEXED_CHECKER2_ROW_PHASES; py++) {
        for (int px = 0; px < GFX_INDEXED_CHECKER2_CHUNK_PX; px++) {
            set_checker2_entry(5, py, px, (gfx_color_t)(0x9000 + py * 2 + px));
        }
    }
    const uint8_t row[6] = {5, 5, 5, 5, 5, 5};

    gfx_color_t whole[12];
    gfx_indexed_expand_row_dither_checker2(row, 6, checker2_table, 2, 3, 0, whole, 12);

    /* Odd split, at an odd starting column - both irregular on purpose. */
    gfx_color_t left[5], right[7];
    gfx_indexed_expand_row_dither_checker2(row, 6, checker2_table, 2, 3, 0, left, 5);
    gfx_indexed_expand_row_dither_checker2(row, 6, checker2_table, 2, 3, 5, right, 7);

    TEST_ASSERT_EQUAL_HEX16_ARRAY(whole, left, 5);
    TEST_ASSERT_EQUAL_HEX16_ARRAY(whole + 5, right, 7);
}

/* the panel row the present path and a screenshot both read */

static gfx_color_t any_mode_table[GFX_INDEXED_PALETTE_SIZE * GFX_INDEXED_DITHER16_PHASES];

/* Each mode's documented table key at panel pixel (x, y), written out per
 * pixel rather than through any expander. */
static gfx_color_t
expected_pixel(const gfx_indexed_frame_t* frame, int x, int y) {
    const int gx = x / frame->cell_size;
    const int gy = y / frame->cell_size;
    const int idx = (gx < frame->grid_w && gy < frame->grid_h) ? frame->image[gy * frame->grid_w + gx] : 0;
    const gfx_color_t* t = frame->table;

    if (!frame->dither16_on) {
        return t[idx];
    }
    switch (frame->dither_mode) {
        case GFX_DITHER_NONE: return t[idx];
        case GFX_DITHER_CELL_CHECKER: return t[idx * 2 + ((gx + gy) & 1)];
        case GFX_DITHER_CELL_BAYER2: return t[idx * 4 + (gy & 1) * 2 + (gx & 1)];
        case GFX_DITHER_PIXEL_CHECKER2: return t[idx * 4 + (y & 1) * 2 + (x & 1)];
        default: return t[idx * 16 + (y & 3) * 4 + (x & 3)];
    }
}

/* 256 colour and every 16-colour pattern, at an odd and an even cell size,
 * over every panel row including the margins right of and below the grid.
 * One table serves every mode, so a wrong pattern, or a grid row passed
 * where a panel row belongs, reads a different entry of it. */
static void
test_panel_row_is_its_modes_own_expansion_in_every_colour_mode(void) {
    for (int i = 0; i < (int)(sizeof any_mode_table / sizeof any_mode_table[0]); i++) {
        any_mode_table[i] = (gfx_color_t)((uint32_t)i * 2654435761u >> 16);
    }

    enum { GRID_W = 7, GRID_H = 5, OUT_W = 32 };

    uint8_t image[GRID_W * GRID_H];
    for (int i = 0; i < GRID_W * GRID_H; i++) {
        image[i] = (uint8_t)(i * 37 + 11);
    }
    static const int cell_sizes[] = {3, 4};

    for (size_t c = 0; c < sizeof cell_sizes / sizeof cell_sizes[0]; c++) {
        for (int m = -1; m < GFX_DITHER_MODE_COUNT; m++) {
            const gfx_indexed_frame_t frame = {
                .image = image,
                .grid_w = GRID_W,
                .grid_h = GRID_H,
                .cell_size = cell_sizes[c],
                .dither16_on = m >= 0,
                .dither_mode = m >= 0 ? (gfx_dither_mode_t)m : GFX_DITHER_NONE,
                .table = any_mode_table,
            };
            for (int y = 0; y < (GRID_H + 2) * cell_sizes[c]; y++) {
                gfx_color_t expected[OUT_W], actual[OUT_W];
                for (int x = 0; x < OUT_W; x++) {
                    expected[x] = expected_pixel(&frame, x, y);
                }
                gfx_indexed_expand_panel_row(&frame, y, actual, OUT_W);
                TEST_ASSERT_EQUAL_HEX16_ARRAY(expected, actual, OUT_W);
            }
        }
    }
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
    RUN_TEST(test_classify_groups_indices_with_an_identical_dither_row);
    RUN_TEST(test_classify_gives_every_index_its_own_class_when_all_rows_differ);
    RUN_TEST(test_classify_names_a_class_after_its_smallest_member);
    RUN_TEST(test_incremental_16_colour_output_matches_a_full_reexpansion);
    RUN_TEST(test_incremental_256_index_output_matches_a_full_reexpansion);
    RUN_TEST(test_needs_repaint_ignores_unchanged_index_when_forced);
    RUN_TEST(test_needs_repaint_matches_cell_changed_when_not_forced);
    RUN_TEST(test_cell_checker_phase_is_gx_plus_cy_parity);
    RUN_TEST(test_cell_bayer2_phase_is_2x2_cell_position);
    RUN_TEST(test_cell_null_row_reads_background_phase);
    RUN_TEST(test_cell_dither_changed_is_exact_per_cell_not_every_phase);
    RUN_TEST(test_repaint_force_full_widens_every_kind);
    RUN_TEST(test_repaint_unforced_unmoved_index_is_cheap_and_false);
    RUN_TEST(test_repaint_raw_kind_is_index_inequality);
    RUN_TEST(test_repaint_class_kind_matches_cell_changed);
    RUN_TEST(test_repaint_cell_kinds_match_cell_dither_changed);
    RUN_TEST(test_checker2_every_output_pixel_reads_its_own_phase_entry);
    RUN_TEST(test_checker2_stays_in_phase_across_a_band_boundary);
    RUN_TEST(test_panel_row_is_its_modes_own_expansion_in_every_colour_mode);
}

SUITE_REGISTER(run_gfx_indexed_suite);

/*
 * Portable suite: paint_row_n() and update_local_depth_gravity()
 * (sand_paint_row.h), called directly rather than mirrored - the paint-row
 * split (AGENTS.md) is what makes app_sand.c's exact hot loop host-linkable
 * at all. suite_sand_liquid_depth.c still mirrors the algorithm for its own,
 * older tests; this file exercises the real thing.
 */
#include <stdlib.h>
#include <string.h>

#include "suites.h"
#include "unity.h"

#include "apps/sand/material_palette.h"
#include "apps/sand/sand_paint_row.h"
#include "apps/sand/tests/suite_sand_common.h"
#include "gfx/gfx.h"

#define PAINT_ROW_TEST_W    4
#define PAINT_ROW_TEST_H    40
#define PAINT_ROW_WATER_TOP 2

/* Paints every row of `cells` (grid_w x grid_h) `passes` times under
 * straight-down gravity, settling the local-depth carry the same way
 * app_sand.c's own frame loop does (see suite_sand_liquid_depth.c's debounce
 * tests for why a fresh boundary needs a second pass to commit).
 *
 * `st` is `static`, matching app_sand.c's own paint_row_state - it holds
 * GRID_W_MAX/GRID_H_MAX arrays sized for the real screen and does not fit a
 * device's small frame-size ceiling as a local. */
static void
paint_settled(const uint8_t* cells, gfx_color_t* fb, int grid_w, int grid_h, int passes) {
    static uint8_t row_a[PAINT_ROW_TEST_W];
    static uint8_t row_b[PAINT_ROW_TEST_W];
    static sand_paint_row_state_t st;
    memset(&st, 0, sizeof st);
    st.local_depth_cur_row = row_a;
    st.local_depth_prev_row = row_b;
    st.local_depth_prev_cy = LOCAL_DEPTH_NO_ROW;

    sand_paint_frame_t pf = {0};
    pf.shine_ux_q8 = 181;
    pf.shine_uy_q8 = 181;
    pf.shine_period = 64;
    pf.wood_leaf_wind_sign = 1;
    pf.wood_leaf_wind_ux_q8 = 256;

    material_set_gravity(0, 1000); /* straight down */

    for (int pass = 0; pass < passes; pass++) {
        update_local_depth_gravity(&st, 0, 1000, grid_w, grid_h);
        for (int cy = 0; cy < grid_h; cy++) {
            const uint8_t* row = cells + cy * grid_w;
            paint_row_n(&st, &pf, fb, NULL, NULL, cy, row, 1, grid_w, grid_h, 0, grid_w, false);
        }
    }
}

/* Shading-and-Colour.md, "Why the interior ignores fill level": a settled
 * liquid interior's own fill level is a solver transient, not a depth cue,
 * so a cell near the pool's surface must paint BRIGHTER than one deep
 * inside it, converging on the body colour as depth saturates - pinned
 * directly against material_colours() by
 * test_a_liquid_interior_is_shaded_by_depth (suite_sand_liquid_depth.c).
 * This test pins the same property through the real paint_row_n() row
 * loop instead of a mirror. */
static void
test_paint_row_n_shades_a_settled_pool_by_depth(void) {
    uint8_t* cells = malloc((size_t)PAINT_ROW_TEST_W * PAINT_ROW_TEST_H);
    TEST_ASSERT_NOT_NULL(cells);
    memset(cells, CELL_EMPTY, (size_t)PAINT_ROW_TEST_W * PAINT_ROW_TEST_H);
    for (int y = PAINT_ROW_WATER_TOP; y < PAINT_ROW_TEST_H; y++) {
        for (int x = 0; x < PAINT_ROW_TEST_W; x++) {
            cells[y * PAINT_ROW_TEST_W + x] = CELL_MAKE(MAT_WATER, MASS_MAX);
        }
    }

    gfx_color_t* fb = malloc((size_t)PAINT_ROW_TEST_H * GFX_WIDTH * sizeof(gfx_color_t));
    TEST_ASSERT_NOT_NULL(fb);

    paint_settled(cells, fb, PAINT_ROW_TEST_W, PAINT_ROW_TEST_H, 2);

    enum { CX = 1 };

    /* The first fully-interior row: WATER_TOP itself is the rim (open air
     * above it), so its own fill level would confound this comparison. */
    const int shallow_cy = PAINT_ROW_WATER_TOP + 1;
    const int deep_cy = PAINT_ROW_TEST_H - 1;

    const gfx_color_t shallow_px = fb[shallow_cy * GFX_WIDTH + CX];
    const gfx_color_t deep_px = fb[deep_cy * GFX_WIDTH + CX];

    TEST_ASSERT_TRUE_MESSAGE(panel_luminance(shallow_px) > panel_luminance(deep_px),
                             "a cell just below the pool's surface must paint BRIGHTER than one "
                             "near the bottom of a deep, settled pool - local depth is the only "
                             "cue an interior liquid cell has left once its own fill level is "
                             "ignored (Shading-and-Colour.md, \"Why the interior ignores fill "
                             "level\")");

    const gfx_color_t body = material_palette()[CELL_MAKE(MAT_WATER, MASS_MAX)];
    TEST_ASSERT_EQUAL_MESSAGE(body, deep_px,
                              "a cell deep enough to saturate local depth must paint EXACTLY the "
                              "body colour, the same guarantee "
                              "test_a_liquid_interior_is_shaded_by_depth pins directly against "
                              "material_colours()");

    free(fb);
    free(cells);
}

void
run_sand_paint_row_suite(void) {
    RUN_TEST(test_paint_row_n_shades_a_settled_pool_by_depth);
}

SUITE_REGISTER(run_sand_paint_row_suite);

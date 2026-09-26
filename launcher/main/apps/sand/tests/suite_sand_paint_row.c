/* Exercises the row painter and its local-depth carry. */
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

static void
paint_settled(const uint8_t* cells, gfx_color_t* fb, int grid_w, int grid_h, int passes) {
    sand_paint_row_state_t* st = malloc(sizeof(*st));
    TEST_ASSERT_NOT_NULL(st);
    sand_paint_row_state_init(st);

    sand_paint_frame_t pf = {0};
    pf.shine_ux_q8 = 181;
    pf.shine_uy_q8 = 181;
    pf.wood_leaf_wind_sign = 1;
    pf.wood_leaf_wind_ux_q8 = 256;

    material_set_gravity(0, 1000); /* straight down */

    for (int pass = 0; pass < passes; pass++) {
        sand_paint_update_local_depth_gravity(st, 0, 1000, grid_w, grid_h);
        for (int cy = 0; cy < grid_h; cy++) {
            const uint8_t* row = cells + cy * grid_w;
            sand_paint_row_n(st, &pf, fb, NULL, cy, row, 1, grid_w, grid_h, 0, grid_w, false);
        }
    }
    material_set_gravity(0, 0);
    free(st);
}

/* A settled pool brightens toward its surface and reaches the body colour
 * at saturated depth. See Shading-and-Colour.md, "Why the interior ignores
 * fill level". */
static void
test_sand_paint_row_n_shades_a_settled_pool_by_depth(void) {
    uint8_t cells[PAINT_ROW_TEST_W * PAINT_ROW_TEST_H];
    memset(cells, CELL_EMPTY, sizeof(cells));
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
    free(fb);

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
}

void
run_sand_paint_row_suite(void) {
    RUN_TEST(test_sand_paint_row_n_shades_a_settled_pool_by_depth);
}

SUITE_REGISTER(run_sand_paint_row_suite);

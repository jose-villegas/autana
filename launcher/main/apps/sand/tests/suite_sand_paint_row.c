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

static sand_paint_frame_t
paint_frame(void) {
    sand_paint_frame_t pf = {0};
    pf.shine_ux_q8 = 256;
    pf.wood_leaf_wind_sign = 1;
    pf.wood_leaf_wind_ux_q8 = 256;
    pf.repaint_kind = GFX_INDEXED_REPAINT_RAW;
    return pf;
}

static sand_paint_row_state_t*
paint_state(int gx, int gy, int w, int h) {
    sand_paint_row_state_t* st = malloc(sizeof(*st));
    TEST_ASSERT_NOT_NULL(st);
    sand_paint_row_state_init(st);
    sand_paint_update_local_depth_gravity(st, gx, gy, w, h);
    return st;
}

static void
paint_one(sand_paint_row_state_t* st, const sand_paint_frame_t* pf, gfx_color_t* fb, uint8_t* index_row, int cy,
          const uint8_t* cells, int n, int w, int h, int x0, int x1, bool force_full) {
    sand_paint_row_n(st, pf, fb, index_row, cy, cells + cy * w, n, w, h, x0, x1, force_full);
}

static void
test_sand_paint_row_flags_cover_full_row_and_clear_on_repaint(void) {
    enum { PAINT_W = 7 };

    uint8_t cells[PAINT_W] = {CELL_MAKE(MAT_WATER, MASS_MAX),
                              MATX(MATX_LEAF),
                              CELL_MAKE(MAT_SAND, SAND_CULLET_BASE),
                              CELL_MAKE(MAT_GLASS, 0),
                              MATX(MATX_METAL),
                              CELL_EMPTY,
                              MATX(MATX_METAL)};
    uint8_t indices[PAINT_W] = {0};
    sand_paint_frame_t pf = paint_frame();
    sand_paint_row_state_t* st = paint_state(0, 1000, PAINT_W, 1);
    paint_one(st, &pf, NULL, indices, 0, cells, 4, PAINT_W, 1, 3, 4, true);
    TEST_ASSERT_EQUAL_HEX8(SAND_PAINT_ROW_FLAG_LIQUID | SAND_PAINT_ROW_FLAG_WOOD_LEAF | SAND_PAINT_ROW_FLAG_CULLET
                               | SAND_PAINT_ROW_FLAG_GLASS | SAND_PAINT_ROW_FLAG_SHINE,
                           st->row_flags[0]);
    TEST_ASSERT_EQUAL(0, st->row_flag_x0[0]);
    TEST_ASSERT_EQUAL(PAINT_W, st->row_flag_x1[0]);
    memset(cells, CELL_EMPTY, sizeof(cells));
    paint_one(st, &pf, NULL, indices, 0, cells, 4, PAINT_W, 1, 3, 4, true);
    TEST_ASSERT_EQUAL(0, st->row_flags[0]);
    TEST_ASSERT_EQUAL(PAINT_W, st->row_flag_x0[0]);
    TEST_ASSERT_EQUAL(0, st->row_flag_x1[0]);
    cells[0] = CELL_MAKE(MAT_WATER, MASS_MAX);
    paint_one(st, &pf, NULL, indices, 0, cells, 4, PAINT_W, 1, 3, 4, true);
    TEST_ASSERT_EQUAL_HEX8(SAND_PAINT_ROW_FLAG_LIQUID, st->row_flags[0]);
    TEST_ASSERT_EQUAL(0, st->row_flag_x0[0]);
    TEST_ASSERT_EQUAL(1, st->row_flag_x1[0]);
    cells[0] = CELL_EMPTY;
    cells[PAINT_W - 1] = MATX(MATX_METAL);
    paint_one(st, &pf, NULL, indices, 0, cells, 4, PAINT_W, 1, 3, 4, true);
    TEST_ASSERT_EQUAL_HEX8(SAND_PAINT_ROW_FLAG_SHINE, st->row_flags[0]);
    TEST_ASSERT_EQUAL(PAINT_W - 1, st->row_flag_x0[0]);
    TEST_ASSERT_EQUAL(PAINT_W, st->row_flag_x1[0]);
    free(st);
}

static void
test_sand_paint_cullet_flags_stop_at_last_shade(void) {
    enum { PAINT_W = 4 };

    uint8_t cells[PAINT_W] = {CELL_MAKE(MAT_SAND, SAND_CULLET_BASE - 1), CELL_MAKE(MAT_SAND, SAND_CULLET_BASE),
                              CELL_MAKE(MAT_SAND, SAND_CULLET_BASE + SAND_CULLET_SHADES - 1),
                              CELL_MAKE(MAT_SAND + 1, 0)};
    uint8_t indices[PAINT_W] = {0};
    sand_paint_frame_t pf = paint_frame();
    sand_paint_row_state_t* st = paint_state(0, 1000, PAINT_W, 1);
    paint_one(st, &pf, NULL, indices, 0, cells, 4, PAINT_W, 1, 0, PAINT_W, true);
    TEST_ASSERT_EQUAL_HEX8(SAND_PAINT_ROW_FLAG_CULLET | SAND_PAINT_ROW_FLAG_LIQUID, st->row_flags[0]);
    TEST_ASSERT_EQUAL(1, st->row_flag_x0[0]);
    TEST_ASSERT_EQUAL(4, st->row_flag_x1[0]);
    cells[1] = cells[0];
    cells[2] = cells[0];
    paint_one(st, &pf, NULL, indices, 0, cells, 4, PAINT_W, 1, 0, PAINT_W, true);
    TEST_ASSERT_EQUAL_HEX8(SAND_PAINT_ROW_FLAG_LIQUID, st->row_flags[0]);
    cells[1] = CELL_MAKE(MAT_SAND, SAND_CULLET_BASE);
    paint_one(st, &pf, NULL, indices, 0, cells, 4, PAINT_W, 1, 0, PAINT_W, true);
    TEST_ASSERT_EQUAL_HEX8(SAND_PAINT_ROW_FLAG_CULLET | SAND_PAINT_ROW_FLAG_LIQUID, st->row_flags[0]);
    TEST_ASSERT_EQUAL(1, st->row_flag_x0[0]);
    cells[1] = cells[0];
    cells[2] = CELL_MAKE(MAT_SAND, SAND_CULLET_BASE + SAND_CULLET_SHADES - 1);
    paint_one(st, &pf, NULL, indices, 0, cells, 4, PAINT_W, 1, 0, PAINT_W, true);
    TEST_ASSERT_EQUAL_HEX8(SAND_PAINT_ROW_FLAG_CULLET | SAND_PAINT_ROW_FLAG_LIQUID, st->row_flags[0]);
    TEST_ASSERT_EQUAL(2, st->row_flag_x0[0]);
    free(st);
}

static void
test_sand_paint_indexed_repaint_tracks_only_changed_in_span(void) {
    enum { PAINT_W = 4 };

    uint8_t cells[PAINT_W] = {MATX(MATX_METAL), MATX(MATX_METAL), MATX(MATX_METAL), MATX(MATX_METAL)};
    uint8_t indices[PAINT_W] = {241, 242, 243, 244};
    sand_paint_frame_t pf = paint_frame();
    sand_paint_row_state_t* st = paint_state(0, 1000, PAINT_W, 1);
    pf.shine_offset = 32;
    paint_one(st, &pf, NULL, indices, 0, cells, 4, PAINT_W, 1, 1, 3, true);
    TEST_ASSERT_EQUAL(241, indices[0]);
    TEST_ASSERT_EQUAL(244, indices[3]);
    TEST_ASSERT_EQUAL(1, st->row_changed_x0[0]);
    TEST_ASSERT_EQUAL(3, st->row_changed_x1[0]);
    const uint8_t off_band = indices[1];
    paint_one(st, &pf, NULL, indices, 0, cells, 4, PAINT_W, 1, 1, 3, false);
    TEST_ASSERT_EQUAL(PAINT_W, st->row_changed_x0[0]);
    TEST_ASSERT_EQUAL(0, st->row_changed_x1[0]);
    paint_one(st, &pf, NULL, indices, 0, cells, 4, PAINT_W, 1, 1, 3, true);
    TEST_ASSERT_EQUAL(1, st->row_changed_x0[0]);
    TEST_ASSERT_EQUAL(3, st->row_changed_x1[0]);
    TEST_ASSERT_EQUAL(241, indices[0]);
    TEST_ASSERT_EQUAL(244, indices[3]);
    gfx_color_t col[3];
    material_colours(cells[1], material_grain_hash(1, 0), 0, 0, col);
    TEST_ASSERT_EQUAL(material_palette256_index(col[0]), off_band);
    pf.shine_offset = 0;
    paint_one(st, &pf, NULL, indices, 0, cells, 4, PAINT_W, 1, 0, 1, true);
    TEST_ASSERT_EQUAL(244, indices[3]);
    material_colours(cells[0], material_grain_hash(0, 0), 0, 0, col);
    TEST_ASSERT_EQUAL(material_palette256_index(col[2]), indices[0]);
    TEST_ASSERT_NOT_EQUAL(off_band, indices[0]);
    free(st);
}

static void
test_sand_paint_gravity_resets_only_for_observable_flips(void) {
    sand_paint_row_state_t* st = paint_state(1, 8, 4, 8);
    st->local_depth_rows[0][0] = 7;
    st->local_depth_prev_cy = 2;
    sand_paint_update_local_depth_gravity(st, -1, 8, 4, 8);
    TEST_ASSERT_EQUAL(SAND_PAINT_NO_ROW, st->local_depth_prev_cy);
    TEST_ASSERT_EQUAL(0, st->local_depth_rows[0][0]);
    st->local_depth_prev_cy = 2;
    st->local_depth_rows[0][0] = 7;
    sand_paint_update_local_depth_gravity(st, 1, 100, 4, 8);
    TEST_ASSERT_EQUAL(2, st->local_depth_prev_cy);
    TEST_ASSERT_EQUAL(7, st->local_depth_rows[0][0]);
    sand_paint_update_local_depth_gravity(st, 4, 1, 4, 8);
    TEST_ASSERT_EQUAL(SAND_PAINT_NO_ROW, st->local_depth_prev_cy);
    st->local_depth_prev_cy = 2;
    sand_paint_update_local_depth_gravity(st, 4, -1, 4, 8);
    TEST_ASSERT_EQUAL(SAND_PAINT_NO_ROW, st->local_depth_prev_cy);
    st->local_depth_prev_cy = 2;
    st->local_depth_rows[0][0] = 7;
    sand_paint_update_local_depth_gravity(st, 100, 1, 4, 8);
    TEST_ASSERT_EQUAL(2, st->local_depth_prev_cy);
    TEST_ASSERT_EQUAL(7, st->local_depth_rows[0][0]);
    sand_paint_update_local_depth_gravity(st, 0, 0, 4, 8);
    TEST_ASSERT_EQUAL(256, st->local_depth_scale_q8);
    sand_paint_update_local_depth_gravity(st, 0, 100, 4, 8);
    TEST_ASSERT_EQUAL(256, st->local_depth_scale_q8);
    sand_paint_update_local_depth_gravity(st, 100, 0, 4, 8);
    TEST_ASSERT_EQUAL(256, st->local_depth_scale_q8);
    sand_paint_update_local_depth_gravity(st, 100, 100, 4, 8);
    TEST_ASSERT_GREATER_THAN(256, st->local_depth_scale_q8);
    free(st);
}

static gfx_color_t
paint_pool_sample(int gx, int gy, int sample_x, int sample_y, int passes) {
    enum { PAINT_W = 32, PAINT_H = 32 };

    uint8_t* cells = malloc(PAINT_W * PAINT_H);
    TEST_ASSERT_NOT_NULL(cells);
    memset(cells, CELL_MAKE(MAT_WATER, MASS_MAX), PAINT_W * PAINT_H);
    gfx_color_t* fb = malloc((size_t)PAINT_H * GFX_WIDTH * sizeof(*fb));
    TEST_ASSERT_NOT_NULL(fb);
    sand_paint_frame_t pf = paint_frame();
    sand_paint_row_state_t* st = paint_state(gx, gy, PAINT_W, PAINT_H);
    for (int pass = 0; pass < passes; pass++) {
        for (int i = 0; i < PAINT_H; i++) {
            const int cy = gy < 0 ? PAINT_H - 1 - i : i;
            paint_one(st, &pf, fb, NULL, cy, cells, 1, PAINT_W, PAINT_H, 0, PAINT_W, true);
        }
    }
    const gfx_color_t result = fb[sample_y * GFX_WIDTH + sample_x];
    free(cells);
    free(st);
    free(fb);
    return result;
}

static void
test_sand_paint_pool_depth_follows_gravity_surface(void) {
    const gfx_color_t body = material_palette()[CELL_MAKE(MAT_WATER, MASS_MAX)];
    TEST_ASSERT_TRUE(panel_luminance(paint_pool_sample(0, -1000, 16, 30, 2))
                     > panel_luminance(paint_pool_sample(0, -1000, 16, 1, 2)));
    TEST_ASSERT_EQUAL(body, paint_pool_sample(0, -1000, 16, 1, 2));
    TEST_ASSERT_TRUE(panel_luminance(paint_pool_sample(1000, 0, 1, 16, 2))
                     > panel_luminance(paint_pool_sample(1000, 0, 30, 16, 2)));
    TEST_ASSERT_EQUAL(body, paint_pool_sample(1000, 0, 30, 16, 2));
    TEST_ASSERT_TRUE(panel_luminance(paint_pool_sample(-1000, 0, 30, 16, 2))
                     > panel_luminance(paint_pool_sample(-1000, 0, 1, 16, 2)));
    TEST_ASSERT_EQUAL(body, paint_pool_sample(-1000, 0, 1, 16, 2));
    TEST_ASSERT_TRUE(panel_luminance(paint_pool_sample(200, 1000, 16, 1, 2))
                     > panel_luminance(paint_pool_sample(200, 1000, 16, 30, 2)));
    TEST_ASSERT_EQUAL(body, paint_pool_sample(200, 1000, 16, 30, 2));
}

static void
test_sand_paint_skipped_row_breaks_carry_then_converges(void) {
    enum { PAINT_W = 3, PAINT_H = 3 };

    uint8_t cells[PAINT_W * PAINT_H];
    memset(cells, CELL_MAKE(MAT_WATER, MASS_MAX), sizeof(cells));
    for (int cx = 0; cx < PAINT_W; cx++) {
        cells[2 * PAINT_W + cx] = CELL_MAKE(MAT_OIL, MASS_MAX);
    }
    uint8_t indices[PAINT_W] = {0};
    sand_paint_frame_t pf = paint_frame();
    sand_paint_row_state_t* st = paint_state(0, 1000, PAINT_W, PAINT_H);
    paint_one(st, &pf, NULL, indices, 0, cells, 4, PAINT_W, PAINT_H, 0, PAINT_W, true);
    paint_one(st, &pf, NULL, indices, 2, cells, 4, PAINT_W, PAINT_H, 0, PAINT_W, true);
    TEST_ASSERT_EQUAL(1, st->local_depth_rows[st->local_depth_cur_index ^ 1u][1]);
    free(st);
    st = paint_state(0, 1000, PAINT_W, PAINT_H);
    paint_one(st, &pf, NULL, indices, 0, cells, 4, PAINT_W, PAINT_H, 0, PAINT_W, true);
    paint_one(st, &pf, NULL, indices, 1, cells, 4, PAINT_W, PAINT_H, 0, PAINT_W, true);
    paint_one(st, &pf, NULL, indices, 2, cells, 4, PAINT_W, PAINT_H, 0, PAINT_W, true);
    TEST_ASSERT_EQUAL(3, st->local_depth_rows[st->local_depth_cur_index ^ 1u][1]);
    paint_one(st, &pf, NULL, indices, 0, cells, 4, PAINT_W, PAINT_H, 0, PAINT_W, true);
    paint_one(st, &pf, NULL, indices, 1, cells, 4, PAINT_W, PAINT_H, 0, PAINT_W, true);
    paint_one(st, &pf, NULL, indices, 2, cells, 4, PAINT_W, PAINT_H, 0, PAINT_W, true);
    const unsigned settled = st->local_depth_rows[st->local_depth_cur_index ^ 1u][1];
    const uint8_t settled_index = indices[1];
    paint_one(st, &pf, NULL, indices, 0, cells, 4, PAINT_W, PAINT_H, 0, PAINT_W, true);
    paint_one(st, &pf, NULL, indices, 1, cells, 4, PAINT_W, PAINT_H, 0, PAINT_W, true);
    paint_one(st, &pf, NULL, indices, 2, cells, 4, PAINT_W, PAINT_H, 0, PAINT_W, true);
    TEST_ASSERT_EQUAL(settled, st->local_depth_rows[st->local_depth_cur_index ^ 1u][1]);
    TEST_ASSERT_EQUAL(settled_index, indices[1]);
    free(st);
}

static void
paint_rgb_blocks_row(sand_paint_row_state_t* st, sand_paint_frame_t* pf, gfx_color_t* fb, const uint8_t* cells, int n) {
    enum { PAINT_W = 4, MAX_N = 8 };

    pf->shine_offset = SAND_PAINT_SHINE_PERIOD - n - 1;
    for (int p = 0; p < MAX_N * GFX_WIDTH; p++) {
        fb[p] = 0x1234;
    }
    paint_one(st, pf, fb, NULL, 0, cells, n, PAINT_W, 1, 1, 3, true);
}

static void
assert_rgb_block_span(const gfx_color_t* fb, int n) {
    enum { PAINT_W = 4, MAX_N = 8 };

    for (int dy = 0; dy < MAX_N; dy++) {
        for (int x = 0; x < PAINT_W * n + 1; x++) {
            const gfx_color_t px = fb[dy * GFX_WIDTH + x];
            if (dy >= n || x < n || x >= 3 * n) {
                TEST_ASSERT_EQUAL_HEX16(0x1234, px);
            } else {
                TEST_ASSERT_NOT_EQUAL(0x1234, px);
            }
        }
    }
}

static void
assert_rgb_metal_shine(const gfx_color_t* fb, const uint8_t* cells, int n) {
    gfx_color_t metal_base = 0;
    gfx_color_t metal_shine = 0;
    for (int dy = 0; dy < n; dy++) {
        for (int x = n; x < 2 * n; x++) {
            const gfx_color_t px = fb[dy * GFX_WIDTH + x];
            if (x == n) {
                metal_base = px;
            }
            if (x == n + n - 1) {
                metal_shine = px;
            }
        }
    }
    TEST_ASSERT_NOT_EQUAL(metal_base, metal_shine);
    gfx_color_t col[3];
    material_colours(cells[1], material_grain_hash(1, 0), 0, 0, col);
    TEST_ASSERT_EQUAL_HEX16(col[0], metal_base);
    TEST_ASSERT_EQUAL_HEX16(col[2], metal_shine);
}

static void
test_sand_paint_rgb_blocks_respect_cell_size_and_partial_span(void) {
    enum { PAINT_W = 4, MAX_N = 8 };

    static const int sizes[] = {2, 3, 4, 6, 8};
    uint8_t cells[PAINT_W] = {CELL_MAKE(MAT_GLASS, 0), MATX(MATX_METAL), CELL_MAKE(MAT_GLASS, 0), CELL_EMPTY};
    gfx_color_t* fb = malloc((size_t)MAX_N * GFX_WIDTH * sizeof(*fb));
    TEST_ASSERT_NOT_NULL(fb);
    sand_paint_frame_t pf = paint_frame();
    sand_paint_row_state_t* st = paint_state(0, 1000, PAINT_W, 1);
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        const int n = sizes[i];
        paint_rgb_blocks_row(st, &pf, fb, cells, n);
        assert_rgb_block_span(fb, n);
        assert_rgb_metal_shine(fb, cells, n);
    }
    free(st);
    free(fb);
}

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
test_sand_paint_row_n_shades_a_settled_pool_by_depth_after_two_passes(void) {
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
    RUN_TEST(test_sand_paint_row_n_shades_a_settled_pool_by_depth_after_two_passes);
    RUN_TEST(test_sand_paint_row_flags_cover_full_row_and_clear_on_repaint);
    RUN_TEST(test_sand_paint_cullet_flags_stop_at_last_shade);
    RUN_TEST(test_sand_paint_indexed_repaint_tracks_only_changed_in_span);
    RUN_TEST(test_sand_paint_gravity_resets_only_for_observable_flips);
    RUN_TEST(test_sand_paint_pool_depth_follows_gravity_surface);
    RUN_TEST(test_sand_paint_skipped_row_breaks_carry_then_converges);
    RUN_TEST(test_sand_paint_rgb_blocks_respect_cell_size_and_partial_span);
}

SUITE_REGISTER(run_sand_paint_row_suite);

/*
 * Portable suite: the GFX_PIXFMT_INDEXED8 pipeline - material_colours()'s
 * body colour mapped to a sand_palette256_lut index (material_palette.c)
 * and back out through gfx_indexed_expand_row() (gfx_indexed.h). Pure and
 * host-portable, the same reason suite_sand_*.c can drive the real
 * simulation at all - see suite_sand_common.h's own top comment.
 *
 * Does not share app_sand.c's own paint_row_indexed_n(): that file is not
 * host-portable (app_*.c, see run_tests.sh). This exercises the pieces it
 * calls - material_colours(), material_palette256_index(),
 * gfx_indexed_expand_row() - against real settled scenes instead.
 */

#include "suites.h"
#include "unity.h"

#include "gfx/gfx_indexed.h"
#include "material.h"
#include "material_palette.h"
#include "sand.h"
#include "sand_palette256.h"

/* An OKLab dE around 2 is "just noticeable" per the palette study
 * (docs/sand/Shading-and-Colour.md); this is a plain RGB888 squared-channel
 * bound, coarser than dE but cheap enough for a test sweep, set generously
 * above the study's own worst measured case (2.85) so it catches a real
 * regression rather than the metric's own difference from dE. */
#define MAX_CHANNEL_ERROR_SQ (20 * 20 * 3)

/* Excludes MAT_EXTENDED variants naming no real static (the study's own
 * "spare" group - zero palette budget, since nothing ever produces one) so
 * the sweep below only visits bytes real gameplay can actually hold. */
static bool
is_real_cell(cell_t c) {
    if (CELL_IS_EMPTY(c)) {
        return false;
    }
    if (CELL_MATERIAL(c) != MAT_EXTENDED) {
        return true;
    }
    if (cell_is_gunpowder(c)) {
        return true;
    }
    const uint8_t v = CELL_VARIANT(c);
    return v == MATX_ICE || v == MATX_PLANT || v == MATX_LEAF || v == MATX_METAL || v == MATX_ROOT;
}

static long
channel_error_sq(gfx_color_t a, gfx_color_t b) {
    const uint32_t ca = gfx_color_rgb888(a), cb = gfx_color_rgb888(b);
    const long dr = (long)((ca >> 16) & 0xFFu) - (long)((cb >> 16) & 0xFFu);
    const long dg = (long)((ca >> 8) & 0xFFu) - (long)((cb >> 8) & 0xFFu);
    const long db = (long)(ca & 0xFFu) - (long)(cb & 0xFFu);
    return dr * dr + dg * dg + db * db;
}

/* Every (material, variant) FLAT/SPECKLED/HATCHED body colour the sim can
 * actually produce at rest (hash 0 and 255, mask 0, depth 0) maps to a
 * sand entry within the study's own error budget - the sweep half of "the
 * index image plus LUT expansion equals the study's quantised render". */
static void
test_every_material_bytes_body_colour_maps_within_budget(void) {
    for (unsigned c = 0; c < 256u; c++) {
        if (!is_real_cell((cell_t)c)) {
            continue;
        }
        for (unsigned hash = 0; hash <= 255u; hash += 255u) {
            gfx_color_t out[3];
            material_colours((cell_t)c, hash, 0u, 0u, out);

            const int idx = material_palette256_index(out[0]);
            TEST_ASSERT_GREATER_OR_EQUAL_INT(SAND_PALETTE_UI_ENTRIES, idx);
            TEST_ASSERT_LESS_THAN_INT(GFX_INDEXED_PALETTE_SIZE, idx);

            const long err = channel_error_sq(out[0], sand_palette256_lut[idx]);
            TEST_ASSERT_LESS_OR_EQUAL_INT_MESSAGE(MAX_CHANNEL_ERROR_SQ, (int)err,
                                                  "a material's body colour drifted out of budget");
        }
    }
}

/* gfx_indexed_expand_row() reproduces the installed LUT exactly, cell for
 * cell - the present-side half of the same equivalence, using the real
 * generated palette rather than a synthetic one (suite_gfx_indexed.c
 * already covers the general upscale/margin arithmetic). */
static void
test_expansion_reproduces_the_real_lut_cell_for_cell(void) {
    const uint8_t row[4] = {SAND_PALETTE_UI_ENTRIES, 40, 200, 255};
    gfx_color_t out[4 * 3];

    gfx_indexed_expand_row(row, 4, sand_palette256_lut, 3, out, 4 * 3);

    for (int gx = 0; gx < 4; gx++) {
        for (int dx = 0; dx < 3; dx++) {
            TEST_ASSERT_EQUAL_HEX16(sand_palette256_lut[row[gx]], out[gx * 3 + dx]);
        }
    }
}

#define SCENE_W 40
#define SCENE_H 30

static uint8_t scene_grid[SCENE_W * SCENE_H];
static sand_t scene_sim;

typedef struct {
    const char* name;
    void (*build)(sand_t* s);
} scene_case_t;

static void
scene_mixed_pile(sand_t* s) {
    for (int x = 0; x < SCENE_W; x++) {
        sand_set(s, x, SCENE_H - 1, CELL_MAKE(MAT_STONE, SAND_AMBIENT_HEAT));
    }
    for (int i = 0; i < 120; i++) {
        sand_spawn(s, 8 + (i % 6), 2, 2, MAT_SAND);
        sand_spawn_cell(s, 30, 2, 2, CELL_MAKE(MAT_WATER, 0));
        sand_step(s, 0, 1000, 0);
    }
    sand_spawn(s, 20, 5, 3, MAT_SNOW);
    sand_spawn(s, 15, 20, 3, MAT_DIRT);
}

static void
scene_glass_and_gas(sand_t* s) {
    for (int y = 5; y < 20; y++) {
        sand_set(s, 10, y, CELL_MAKE(MAT_GLASS, SAND_AMBIENT_HEAT));
        sand_set(s, 11, y, CELL_MAKE(MAT_GLASS, SAND_SHOCK_HEAT));
    }
    sand_spawn(s, 25, 10, 4, MAT_GAS);
    sand_spawn(s, 5, 25, 3, MAT_FIRE);
    for (int i = 0; i < 20; i++) {
        sand_step(s, 0, 1000, 0);
    }
}

static const scene_case_t scenes[] = {
    {"mixed_pile", scene_mixed_pile},
    {"glass_and_gas", scene_glass_and_gas},
};

/* Every settled cell in every scene, at both a fine and a coarse quality's
 * own cell size, maps to a valid sand index and expands back to the exact
 * LUT colour that index names - the integration half of the equivalence,
 * against real (not hand-picked) hash/mask/depth combinations. */
static void
test_settled_scenes_map_and_expand_correctly_at_every_quality(void) {
    static const int cell_sizes[] = {2, 4, 8}; /* ULTRA, NORMAL, VERY LOW */

    for (size_t si = 0; si < sizeof scenes / sizeof scenes[0]; si++) {
        sand_init(&scene_sim, scene_grid, SCENE_W, SCENE_H, 0x51EED000u + (uint32_t)si);
        scenes[si].build(&scene_sim);

        for (int cy = 0; cy < SCENE_H; cy++) {
            const uint8_t* row = &scene_grid[cy * SCENE_W];
            for (int cx = 0; cx < SCENE_W; cx++) {
                if (CELL_IS_EMPTY(row[cx])) {
                    continue;
                }
                const unsigned hash = material_grain_hash(cx, cy);
                const unsigned mask = ((cx > 0 && CELL_IS_EMPTY(row[cx - 1])) ? MATERIAL_EDGE_LEFT : 0u)
                                      | ((cx < SCENE_W - 1 && CELL_IS_EMPTY(row[cx + 1])) ? MATERIAL_EDGE_RIGHT : 0u);

                gfx_color_t out[3];
                material_colours(row[cx], hash, mask, 0u, out);
                const int idx = material_palette256_index(out[0]);

                TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(SAND_PALETTE_UI_ENTRIES, idx, scenes[si].name);
                TEST_ASSERT_LESS_THAN_INT_MESSAGE(GFX_INDEXED_PALETTE_SIZE, idx, scenes[si].name);

                for (size_t q = 0; q < sizeof cell_sizes / sizeof cell_sizes[0]; q++) {
                    gfx_color_t expanded[8];
                    const uint8_t one[1] = {(uint8_t)idx};
                    gfx_indexed_expand_row(one, 1, sand_palette256_lut, cell_sizes[q], expanded, cell_sizes[q]);
                    for (int dx = 0; dx < cell_sizes[q]; dx++) {
                        TEST_ASSERT_EQUAL_HEX16(sand_palette256_lut[idx], expanded[dx]);
                    }
                }
            }
        }
    }
}

void
run_sand_palette256_suite(void) {
    RUN_TEST(test_every_material_bytes_body_colour_maps_within_budget);
    RUN_TEST(test_expansion_reproduces_the_real_lut_cell_for_cell);
    RUN_TEST(test_settled_scenes_map_and_expand_correctly_at_every_quality);
}

SUITE_REGISTER(run_sand_palette256_suite);

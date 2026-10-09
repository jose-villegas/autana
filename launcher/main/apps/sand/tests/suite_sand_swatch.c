/*
 * Portable suite: sand_swatch - the deterministic (col, row) -> cell mapping
 * behind the brush screen's material swatch.
 * Every case runs over the app's own sand_brushes[].
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "suites.h"
#include "unity.h"

#include "apps/sand/material.h"
#include "apps/sand/sand_brushes.h"
#include "apps/sand/sand_swatch.h"

#define SWATCH_CELLS 8

static cell_t
fixture(cell_t spec, int col, int row) {
    return sand_swatch_cell(spec, col, row, SWATCH_CELLS);
}

/* Determinism - what ui_end()'s repaint-skip hash depends on. */

static void
test_the_same_spec_col_row_return_the_same_cell_every_call(void) {
    for (int i = 0; i < SAND_BRUSH_COUNT; i++) {
        for (int row = 0; row < SWATCH_CELLS; row++) {
            for (int col = 0; col < SWATCH_CELLS; col++) {
                const cell_t first = fixture(sand_brushes[i].cell, col, row);
                for (int rep = 0; rep < 5; rep++) {
                    TEST_ASSERT_EQUAL_UINT8_MESSAGE(first, fixture(sand_brushes[i].cell, col, row),
                                                    "a repeated call with the same inputs must return the same "
                                                    "byte, or the command-list hash a repaint skip depends on "
                                                    "would never repeat frame to frame");
                }
            }
        }
    }
}

/* Every variant stays within the material's real shade range. */

static void
test_every_variant_stays_within_the_material_shade_span(void) {
    for (int i = 0; i < SAND_BRUSH_COUNT; i++) {
        const cell_t spec = sand_brushes[i].cell;
        if (cell_is_gunpowder(spec) || cell_is_extended(spec)) {
            continue; /* no shade axis - covered below instead */
        }
        const int span = MATERIAL_SHADE_SPAN((material_id_t)CELL_MATERIAL(spec));
        for (int row = 0; row < SWATCH_CELLS; row++) {
            for (int col = 0; col < SWATCH_CELLS; col++) {
                const cell_t c = fixture(spec, col, row);
                TEST_ASSERT_EQUAL_UINT8_MESSAGE(CELL_MATERIAL(spec), CELL_MATERIAL(c),
                                                "the material identity must never change");
                TEST_ASSERT_TRUE_MESSAGE(CELL_VARIANT(c) < span,
                                         "a shade outside the material's own MATERIAL_SHADE_SPAN() "
                                         "range is not a shade the grid can ever actually render");
            }
        }
    }
}

/* Gunpowder and the MATX() extended materials: no shade axis, flat swatch. */

static void
test_gunpowder_and_extended_cells_yield_a_uniform_swatch(void) {
    const cell_t specs[] = {MATX(MATX_ICE), MATX(MATX_PLANT), GUNPOWDER_CELL(0)};
    for (size_t i = 0; i < sizeof(specs) / sizeof(specs[0]); i++) {
        const cell_t first = fixture(specs[i], 0, 0);
        for (int row = 0; row < SWATCH_CELLS; row++) {
            for (int col = 0; col < SWATCH_CELLS; col++) {
                TEST_ASSERT_EQUAL_UINT8_MESSAGE(first, fixture(specs[i], col, row),
                                                "a material whose low nibble means state, not shade, must "
                                                "paint one flat colour rather than a texture built from "
                                                "bytes that mean something else");
            }
        }
    }
}

/*
 * A multi-shade material actually varies - the texture-is-secretly-flat
 * failure the uniform-swatch test above cannot catch.
 */

static void
test_a_multi_shade_material_uses_more_than_one_variant(void) {
    bool seen[MATERIAL_VARIANTS] = {false};
    int distinct = 0;
    for (int row = 0; row < SWATCH_CELLS; row++) {
        for (int col = 0; col < SWATCH_CELLS; col++) {
            const uint8_t v = CELL_VARIANT(fixture(CELL_MAKE(MAT_STONE, 0), col, row));
            if (!seen[v]) {
                seen[v] = true;
                distinct++;
            }
        }
    }
    TEST_ASSERT_GREATER_THAN_MESSAGE(1, distinct,
                                     "an 8x8 swatch of a 16-shade material showing only one shade is a "
                                     "texture that is secretly flat");
}

/* Every brush spec is handled - nothing indexes off the end. */

static void
test_every_brush_spec_stays_in_range_across_the_whole_grid(void) {
    for (int i = 0; i < SAND_BRUSH_COUNT; i++) {
        for (int row = 0; row < SWATCH_CELLS; row++) {
            for (int col = 0; col < SWATCH_CELLS; col++) {
                const cell_t c = fixture(sand_brushes[i].cell, col, row);
                TEST_ASSERT_TRUE_MESSAGE(CELL_VARIANT(c) <= 0x0F,
                                         "the variant nibble must never spill outside its own 4 bits");
            }
        }
    }
}

void
run_sand_swatch_suite(void) {
    RUN_TEST(test_the_same_spec_col_row_return_the_same_cell_every_call);
    RUN_TEST(test_every_variant_stays_within_the_material_shade_span);
    RUN_TEST(test_gunpowder_and_extended_cells_yield_a_uniform_swatch);
    RUN_TEST(test_a_multi_shade_material_uses_more_than_one_variant);
    RUN_TEST(test_every_brush_spec_stays_in_range_across_the_whole_grid);
}

SUITE_REGISTER(run_sand_swatch_suite);

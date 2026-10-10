/*
 * Portable suite: the app's dither swatches, read from its pack, each
 * against the rule its
 * dither mode follows (gfx_indexed.h), not against the generator: one 4x4
 * period, a cell mode dithering over 2-pixel cells and a pixel mode per
 * pixel, both Bayer swatches at a quarter tone.
 */

#include <stdbool.h>

#include "icon_walk.h"
#include "suites.h"
#include "unity.h"

#include "apps/sand/ui/sand_icons.h"

/* Swatch `mode`, which the app pack the suites run with must hold. */
static const gfx_image_t*
swatch(gfx_dither_mode_t mode) {
    (void)ui_icon_set_load(&sand_icon_set);
    const gfx_image_t* icon = sand_dither_icon(mode);
    TEST_ASSERT_NOT_NULL_MESSAGE(icon, "no swatch: the suites run with the packs build_pack.py writes");
    return icon;
}

static const int BAYER2[2][2] = {{0, 2}, {3, 1}};
static const int BAYER4[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};

static bool
expected(gfx_dither_mode_t id, int x, int y) {
    switch (id) {
        case GFX_DITHER_NONE: return true;
        case GFX_DITHER_CELL_CHECKER: return ((x / 2 + y / 2) & 1) == 0;
        case GFX_DITHER_CELL_BAYER2: return BAYER2[(y / 2) & 1][(x / 2) & 1] < 1;
        case GFX_DITHER_PIXEL_CHECKER2: return ((x + y) & 1) == 0;
        case GFX_DITHER_PIXEL_BAYER4: return BAYER4[y & 3][x & 3] < 4;
        default: return false;
    }
}

static void
test_every_swatch_is_its_modes_own_pattern(void) {
    for (int id = 0; id < GFX_DITHER_MODE_COUNT; id++) {
        const gfx_image_t* icon = swatch((gfx_dither_mode_t)id);
        TEST_ASSERT_EQUAL_INT(4, icon->width);
        TEST_ASSERT_EQUAL_INT(4, icon->height);
        for (int y = 0; y < icon->height; y++) {
            for (int x = 0; x < icon->width; x++) {
                TEST_ASSERT_EQUAL_INT_MESSAGE(expected((gfx_dither_mode_t)id, x, y), icon_test_bit(icon, x, y),
                                              "a swatch left its mode's rule");
            }
        }
    }
}

static void
test_a_cell_swatch_is_the_pixel_swatch_at_twice_the_grain(void) {
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            TEST_ASSERT_EQUAL_INT(icon_test_bit(swatch(GFX_DITHER_PIXEL_CHECKER2), x / 2, y / 2),
                                  icon_test_bit(swatch(GFX_DITHER_CELL_CHECKER), x, y));
        }
    }
}

void
run_sand_dither_icons_suite(void) {
    RUN_TEST(test_every_swatch_is_its_modes_own_pattern);
    RUN_TEST(test_a_cell_swatch_is_the_pixel_swatch_at_twice_the_grain);
    ui_icon_set_release(&sand_icon_set);
}

SUITE_REGISTER(run_sand_dither_icons_suite);

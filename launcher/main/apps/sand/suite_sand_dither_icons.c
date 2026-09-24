/*
 * Portable suite: icons_dither - each baked swatch against the rule its
 * dither mode follows (gfx_indexed.h), not against the generator: one 4x4
 * period, a cell mode dithering over 2-pixel cells and a pixel mode per
 * pixel, both Bayer swatches at a quarter tone.
 */

#include <stdbool.h>

#include "suites.h"
#include "unity.h"

#include "icons_dither.h"

static const int BAYER2[2][2] = {{0, 2}, {3, 1}};
static const int BAYER4[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};

static bool
baked_bit(icon_dither_id_t id, int x, int y) {
    const icon_t* icon = &icon_dither_table[id];
    const uint8_t byte = icon_dither_rows[icon->offset + (unsigned)y * icon->stride + (unsigned)(x / 8)];
    return (byte & (0x80 >> (x % 8))) != 0;
}

static bool
expected(icon_dither_id_t id, int x, int y) {
    switch (id) {
        case ICON_DITHER_NONE: return true;
        case ICON_DITHER_CELL_CHECKER: return ((x / 2 + y / 2) & 1) == 0;
        case ICON_DITHER_CELL_BAYER2: return BAYER2[(y / 2) & 1][(x / 2) & 1] < 1;
        case ICON_DITHER_PIXEL_CHECKER2: return ((x + y) & 1) == 0;
        case ICON_DITHER_PIXEL_BAYER4: return BAYER4[y & 3][x & 3] < 4;
        default: return false;
    }
}

static void
test_every_swatch_is_its_modes_own_pattern(void) {
    for (int id = 0; id < ICON_DITHER_COUNT; id++) {
        const icon_t* icon = &icon_dither_table[id];
        TEST_ASSERT_EQUAL_INT(4, icon->w);
        TEST_ASSERT_EQUAL_INT(4, icon->h);
        for (int y = 0; y < icon->h; y++) {
            for (int x = 0; x < icon->w; x++) {
                TEST_ASSERT_EQUAL_INT_MESSAGE(expected((icon_dither_id_t)id, x, y),
                                              baked_bit((icon_dither_id_t)id, x, y), "a swatch left its mode's rule");
            }
        }
    }
}

static void
test_a_cell_swatch_is_the_pixel_swatch_at_twice_the_grain(void) {
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            TEST_ASSERT_EQUAL_INT(baked_bit(ICON_DITHER_PIXEL_CHECKER2, x / 2, y / 2),
                                  baked_bit(ICON_DITHER_CELL_CHECKER, x, y));
        }
    }
}

void
run_sand_dither_icons_suite(void) {
    RUN_TEST(test_every_swatch_is_its_modes_own_pattern);
    RUN_TEST(test_a_cell_swatch_is_the_pixel_swatch_at_twice_the_grain);
}

SUITE_REGISTER(run_sand_dither_icons_suite);

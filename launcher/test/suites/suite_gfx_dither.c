/*
 * Portable suite: the ordered screen-space threshold patterns used by
 * backdrop rendering.
 */

#include <stdbool.h>
#include <stdint.h>

#include "suites.h"
#include "unity.h"

#include "gfx/draw/gfx_dither.h"

static void
test_every_pattern_visits_each_threshold_once_per_period(void) {
    for (int pattern = 0; pattern < GFX_DITHER_PATTERN_COUNT; pattern++) {
        const gfx_dither_pattern_t* const table = gfx_dither_pattern((gfx_dither_pattern_id_t)pattern);
        uint32_t seen[GFX_DITHER_MAX_LEVELS / 32] = {0};

        TEST_ASSERT_NOT_NULL(table);
        TEST_ASSERT_TRUE(table->levels <= GFX_DITHER_MAX_LEVELS);
        for (int y = 0; y < table->height; y++) {
            for (int x = 0; x < table->width; x++) {
                const int level =
                    gfx_dither_threshold((gfx_dither_pattern_id_t)pattern, x * table->scale_x, y * table->scale_y);
                TEST_ASSERT_TRUE(level >= 0 && level < table->levels);
                TEST_ASSERT_FALSE((seen[level / 32] >> (level & 31)) & 1u);
                seen[level / 32] |= 1u << (level & 31);
            }
        }
        for (int level = 0; level < table->levels; level++) {
            TEST_ASSERT_TRUE((seen[level / 32] >> (level & 31)) & 1u);
        }
    }
}

static void
test_thresholds_are_fixed_to_screen_coordinates(void) {
    for (int pattern = 0; pattern < GFX_DITHER_PATTERN_COUNT; pattern++) {
        const gfx_dither_pattern_t* const table = gfx_dither_pattern((gfx_dither_pattern_id_t)pattern);
        for (int y = 0; y < table->height * 3; y++) {
            for (int x = 0; x < table->width * 3; x++) {
                TEST_ASSERT_EQUAL_INT(
                    gfx_dither_threshold((gfx_dither_pattern_id_t)pattern, x, y),
                    gfx_dither_threshold((gfx_dither_pattern_id_t)pattern, x + table->width * table->scale_x, y));
                TEST_ASSERT_EQUAL_INT(
                    gfx_dither_threshold((gfx_dither_pattern_id_t)pattern, x, y),
                    gfx_dither_threshold((gfx_dither_pattern_id_t)pattern, x, y + table->height * table->scale_y));
            }
        }
    }
}

static void
test_dithered_alpha_selects_its_requested_share(void) {
    for (int pattern = 0; pattern < GFX_DITHER_PATTERN_COUNT; pattern++) {
        const gfx_dither_pattern_t* const table = gfx_dither_pattern((gfx_dither_pattern_id_t)pattern);
        int covered = 0;
        TEST_ASSERT_FALSE(gfx_dither_alpha_pick((gfx_dither_pattern_id_t)pattern, 0, 0, 0));
        TEST_ASSERT_TRUE(gfx_dither_alpha_pick((gfx_dither_pattern_id_t)pattern, 0, 0, 255));
        for (int y = 0; y < table->height; y++) {
            for (int x = 0; x < table->width; x++) {
                covered += gfx_dither_alpha_pick((gfx_dither_pattern_id_t)pattern, x * table->scale_x,
                                                 y * table->scale_y, 128);
            }
        }
        TEST_ASSERT_INT_WITHIN(1, table->levels / 2, covered);
    }
}

void
suite_gfx_dither(void) {
    RUN_TEST(test_every_pattern_visits_each_threshold_once_per_period);
    RUN_TEST(test_thresholds_are_fixed_to_screen_coordinates);
    RUN_TEST(test_dithered_alpha_selects_its_requested_share);
}

SUITE_REGISTER(suite_gfx_dither);

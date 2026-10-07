#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "suites.h"
#include "unity.h"

#include "render/upscale.h"

static void
test_two_times_matches_the_frame_doubling_reference(void) {
    enum { WIDTH = 7, HEIGHT = 5, PIXELS = WIDTH * HEIGHT };

    uint16_t* source = malloc(sizeof(*source) * PIXELS);
    uint16_t* actual = malloc(sizeof(*actual) * 4 * PIXELS);
    uint16_t* columns = malloc(sizeof(*columns) * 2 * WIDTH);
    uint16_t* rows = malloc(sizeof(*rows) * 2 * HEIGHT);
    TEST_ASSERT_NOT_NULL(source);
    TEST_ASSERT_NOT_NULL(actual);
    TEST_ASSERT_NOT_NULL(columns);
    TEST_ASSERT_NOT_NULL(rows);
    for (int i = 0; i < PIXELS; i++) {
        source[i] = (uint16_t)(0x1200 + i);
    }
    upscale_t scale;
    const gfx_render_target_t picture = gfx_render_target_of_color(source, WIDTH, HEIGHT);
    TEST_ASSERT_TRUE(upscale_init(&scale, WIDTH, HEIGHT, 2 * WIDTH, 2 * HEIGHT, columns, rows));
    upscale_rows(&scale, &picture, 0, actual, 0, 2 * HEIGHT);
    for (int y = 0; y < 2 * HEIGHT; y++) {
        for (int x = 0; x < 2 * WIDTH; x++) {
            TEST_ASSERT_EQUAL_HEX16(source[(y / 2) * WIDTH + (x / 2)], actual[y * 2 * WIDTH + x]);
        }
    }
    free(rows);
    free(columns);
    free(actual);
    free(source);
}

static void
test_integer_factors_copy_each_source_pixel_to_its_block(void) {
    static const int factors[] = {1, 2, 4, 8};

    enum { WIDTH = 3, HEIGHT = 2 };

    uint16_t source[WIDTH * HEIGHT] = {1, 2, 3, 4, 5, 6};
    const gfx_render_target_t picture = gfx_render_target_of_color(source, WIDTH, HEIGHT);
    for (size_t i = 0; i < sizeof(factors) / sizeof(factors[0]); i++) {
        const int factor = factors[i];
        const int out_width = factor * WIDTH;
        const int out_height = factor * HEIGHT;
        uint16_t* actual = malloc(sizeof(*actual) * (size_t)out_width * (size_t)out_height);
        uint16_t* columns = malloc(sizeof(*columns) * (size_t)out_width);
        uint16_t* rows = malloc(sizeof(*rows) * (size_t)out_height);
        TEST_ASSERT_NOT_NULL(actual);
        TEST_ASSERT_NOT_NULL(columns);
        TEST_ASSERT_NOT_NULL(rows);
        upscale_t scale;
        TEST_ASSERT_TRUE(upscale_init(&scale, WIDTH, HEIGHT, out_width, out_height, columns, rows));
        upscale_rows(&scale, &picture, 0, actual, 0, out_height);
        for (int y = 0; y < out_height; y++) {
            for (int x = 0; x < out_width; x++) {
                TEST_ASSERT_EQUAL_UINT16(source[(y / factor) * WIDTH + x / factor], actual[y * out_width + x]);
            }
        }
        free(rows);
        free(columns);
        free(actual);
    }
}

static void
test_fractional_maps_cover_destination_in_order_and_at_source_corners(void) {
    uint16_t columns[12], rows[9];
    upscale_t scale;
    TEST_ASSERT_TRUE(upscale_init(&scale, 8, 6, 12, 9, columns, rows));
    TEST_ASSERT_FALSE(scale.integer);
    TEST_ASSERT_EQUAL_UINT16(0, scale.columns[0]);
    TEST_ASSERT_EQUAL_UINT16(7, scale.columns[11]);
    TEST_ASSERT_EQUAL_UINT16(0, scale.rows[0]);
    TEST_ASSERT_EQUAL_UINT16(5, scale.rows[8]);
    for (int x = 0; x < scale.destination_width; x++) {
        TEST_ASSERT_TRUE(scale.columns[x] < scale.source_width);
        if (x != 0) {
            TEST_ASSERT_TRUE(scale.columns[x - 1] <= scale.columns[x]);
        }
    }
    for (int y = 0; y < scale.destination_height; y++) {
        TEST_ASSERT_TRUE(scale.rows[y] < scale.source_height);
        if (y != 0) {
            TEST_ASSERT_TRUE(scale.rows[y - 1] <= scale.rows[y]);
        }
    }
}

static void
test_two_row_ranges_equal_one_whole_upscale(void) {
    enum { WIDTH = 5, HEIGHT = 4, OUT_WIDTH = 8, OUT_HEIGHT = 7 };

    uint16_t source[WIDTH * HEIGHT];
    uint16_t whole[OUT_WIDTH * OUT_HEIGHT], split[OUT_WIDTH * OUT_HEIGHT];
    uint16_t columns[OUT_WIDTH], rows[OUT_HEIGHT];
    for (int i = 0; i < WIDTH * HEIGHT; i++) {
        source[i] = (uint16_t)i;
    }
    upscale_t scale;
    const gfx_render_target_t picture = gfx_render_target_of_color(source, WIDTH, HEIGHT);
    TEST_ASSERT_TRUE(upscale_init(&scale, WIDTH, HEIGHT, OUT_WIDTH, OUT_HEIGHT, columns, rows));
    upscale_rows(&scale, &picture, 0, whole, 0, OUT_HEIGHT);
    upscale_rows(&scale, &picture, 0, split, 0, OUT_HEIGHT / 2);
    upscale_rows(&scale, &picture, 0, split, OUT_HEIGHT / 2, OUT_HEIGHT - OUT_HEIGHT / 2);
    TEST_ASSERT_EQUAL_HEX16_ARRAY(whole, split, OUT_WIDTH * OUT_HEIGHT);
}

void
run_upscale_suite(void) {
    RUN_TEST(test_two_times_matches_the_frame_doubling_reference);
    RUN_TEST(test_integer_factors_copy_each_source_pixel_to_its_block);
    RUN_TEST(test_fractional_maps_cover_destination_in_order_and_at_source_corners);
    RUN_TEST(test_two_row_ranges_equal_one_whole_upscale);
}

SUITE_REGISTER(run_upscale_suite);

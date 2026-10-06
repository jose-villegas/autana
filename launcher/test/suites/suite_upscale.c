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
    TEST_ASSERT_TRUE(upscale_init(&scale, WIDTH, HEIGHT, 2 * WIDTH, 2 * HEIGHT, columns, rows));
    upscale_rows(&scale, source, NULL, 0, actual, 0, 2 * HEIGHT);
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

    const uint16_t source[WIDTH * HEIGHT] = {1, 2, 3, 4, 5, 6};
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
        upscale_rows(&scale, source, NULL, 0, actual, 0, out_height);
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
    TEST_ASSERT_EQUAL_INT(UPSCALE_MAPPED, scale.path);
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
    TEST_ASSERT_TRUE(upscale_init(&scale, WIDTH, HEIGHT, OUT_WIDTH, OUT_HEIGHT, columns, rows));
    upscale_rows(&scale, source, NULL, 0, whole, 0, OUT_HEIGHT);
    upscale_rows(&scale, source, NULL, 0, split, 0, OUT_HEIGHT / 2);
    upscale_rows(&scale, source, NULL, 0, split, OUT_HEIGHT / 2, OUT_HEIGHT - OUT_HEIGHT / 2);
    TEST_ASSERT_EQUAL_HEX16_ARRAY(whole, split, OUT_WIDTH * OUT_HEIGHT);
}

typedef struct {
    int source_width, source_height, destination_width, destination_height;
    upscale_path_t path;
} path_case_t;

/* Every destination pixel is the source pixel the maps name, or 0xF00D
 * where its depth is zero. */
static void
assert_each_pixel_follows_the_maps(const upscale_t* scale, const uint16_t* source, const uint16_t* depth,
                                   const uint16_t* actual) {
    for (int y = 0; y < scale->destination_height; y++) {
        for (int x = 0; x < scale->destination_width; x++) {
            const size_t from = ((size_t)scale->rows[y] * (size_t)scale->source_width) + scale->columns[x];
            const uint16_t expected = depth != NULL && depth[from] == 0 ? 0xF00D : source[from];
            TEST_ASSERT_EQUAL_HEX16(expected, actual[((size_t)y * (size_t)scale->destination_width) + (size_t)x]);
        }
    }
}

/* Upscales in two ranges split on an odd row, with and without depth, and
 * holds every destination pixel to what the maps say it shows. */
static void
assert_matches_the_maps(const path_case_t* c) {
    const size_t source_pixels = (size_t)c->source_width * (size_t)c->source_height;
    const size_t destination_pixels = (size_t)c->destination_width * (size_t)c->destination_height;
    uint16_t* source = malloc(sizeof(*source) * source_pixels);
    uint16_t* depth = malloc(sizeof(*depth) * source_pixels);
    uint16_t* actual = malloc(sizeof(*actual) * destination_pixels);
    uint16_t* columns = malloc(sizeof(*columns) * (size_t)c->destination_width);
    uint16_t* rows = malloc(sizeof(*rows) * (size_t)c->destination_height);
    TEST_ASSERT_NOT_NULL(source);
    TEST_ASSERT_NOT_NULL(depth);
    TEST_ASSERT_NOT_NULL(actual);
    TEST_ASSERT_NOT_NULL(columns);
    TEST_ASSERT_NOT_NULL(rows);
    for (size_t i = 0; i < source_pixels; i++) {
        source[i] = (uint16_t)(0x1000 + i);
        depth[i] = (uint16_t)(i % 3 == 0 ? 0 : i);
    }
    upscale_t scale;
    TEST_ASSERT_TRUE(upscale_init(&scale, c->source_width, c->source_height, c->destination_width,
                                  c->destination_height, columns, rows));
    TEST_ASSERT_EQUAL_INT(c->path, scale.path);
    const int split = (c->destination_height / 2) | 1;
    for (int with_depth = 0; with_depth < 2; with_depth++) {
        const uint16_t* d = with_depth ? depth : NULL;
        memset(actual, 0, sizeof(*actual) * destination_pixels);
        upscale_rows(&scale, source, d, 0xF00D, actual, 0, split);
        upscale_rows(&scale, source, d, 0xF00D, actual, split, c->destination_height - split);
        assert_each_pixel_follows_the_maps(&scale, source, d, actual);
    }
    free(rows);
    free(columns);
    free(actual);
    free(depth);
    free(source);
}

static void
test_a_kept_width_copies_mapped_rows(void) {
    static const path_case_t cases[] = {
        {8, 5, 8, 9, UPSCALE_ROWS},      {8, 6, 8, 12, UPSCALE_ROWS},     {6, 3, 6, 3, UPSCALE_ROWS},
        {92, 56, 92, 112, UPSCALE_ROWS}, {92, 90, 92, 112, UPSCALE_ROWS}, {92, 75, 92, 112, UPSCALE_ROWS},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        assert_matches_the_maps(&cases[i]);
    }
}

static void
test_a_doubled_width_writes_pairs_down_any_row_map(void) {
    static const path_case_t cases[] = {
        {4, 5, 8, 9, UPSCALE_PAIRS},      {4, 3, 8, 7, UPSCALE_PAIRS},       {7, 5, 14, 10, UPSCALE_PAIRS},
        {46, 75, 92, 112, UPSCALE_PAIRS}, {46, 112, 92, 112, UPSCALE_PAIRS}, {46, 56, 92, 112, UPSCALE_PAIRS},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        assert_matches_the_maps(&cases[i]);
    }
}

static void
test_other_fractional_widths_take_the_maps(void) {
    static const path_case_t cases[] = {
        {61, 75, 92, 112, UPSCALE_MAPPED},
        {6, 4, 15, 10, UPSCALE_MAPPED},
        {5, 4, 15, 8, UPSCALE_MAPPED},
        {5, 3, 5, 7, UPSCALE_MAPPED},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        assert_matches_the_maps(&cases[i]);
    }
}

void
run_upscale_suite(void) {
    RUN_TEST(test_two_times_matches_the_frame_doubling_reference);
    RUN_TEST(test_integer_factors_copy_each_source_pixel_to_its_block);
    RUN_TEST(test_fractional_maps_cover_destination_in_order_and_at_source_corners);
    RUN_TEST(test_two_row_ranges_equal_one_whole_upscale);
    RUN_TEST(test_a_kept_width_copies_mapped_rows);
    RUN_TEST(test_a_doubled_width_writes_pairs_down_any_row_map);
    RUN_TEST(test_other_fractional_widths_take_the_maps);
}

SUITE_REGISTER(run_upscale_suite);

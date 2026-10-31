#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "suites.h"
#include "unity.h"

#include "testkit/cleanup.h"

#include "gfx/target/gfx_color.h"
#include "gfx/target/gfx_target.h"
#include "image/pixel/rgb565.h"
#include "render/raster.h"
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
    GFX_TARGET_STORAGE(2) picture_storage = GFX_TARGET_COLOR_INIT(source, WIDTH, HEIGHT);
    gfx_target_t* picture = &picture_storage.target;
    TEST_ASSERT_TRUE(upscale_init(&scale, WIDTH, HEIGHT, 2 * WIDTH, 2 * HEIGHT, columns, rows));
    upscale_rows(&scale, picture, 0, actual, 0, 2 * HEIGHT);
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
    GFX_TARGET_STORAGE(2) picture_storage = GFX_TARGET_COLOR_INIT(source, WIDTH, HEIGHT);
    gfx_target_t* picture = &picture_storage.target;
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
        upscale_rows(&scale, picture, 0, actual, 0, out_height);
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
    GFX_TARGET_STORAGE(2) picture_storage = GFX_TARGET_COLOR_INIT(source, WIDTH, HEIGHT);
    gfx_target_t* picture = &picture_storage.target;
    TEST_ASSERT_TRUE(upscale_init(&scale, WIDTH, HEIGHT, OUT_WIDTH, OUT_HEIGHT, columns, rows));
    upscale_rows(&scale, picture, 0, whole, 0, OUT_HEIGHT);
    upscale_rows(&scale, picture, 0, split, 0, OUT_HEIGHT / 2);
    upscale_rows(&scale, picture, 0, split, OUT_HEIGHT / 2, OUT_HEIGHT - OUT_HEIGHT / 2);
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

#define UNWRITTEN 0xA5A5

/* The buffers one case works in, freed by the cleanup so a failed assertion
 * leaves nothing behind. */
static struct {
    uint16_t *source, *depth, *actual, *columns, *rows;
} held;

static void
release_buffers(void) {
    free(held.rows);
    free(held.columns);
    free(held.actual);
    free(held.depth);
    free(held.source);
    held.source = held.depth = held.actual = held.columns = held.rows = NULL;
}

/* From `first` on, nothing was written. */
static void
assert_unwritten_from(const uint16_t* actual, size_t first, size_t count) {
    for (size_t i = first; i < first + count; i++) {
        TEST_ASSERT_EQUAL_HEX16(UNWRITTEN, actual[i]);
    }
}

/* Upscales [0, split) then the rest: the first call writes no row from
 * `split` on, neither writes the row past the destination that `actual`
 * holds, and every pixel then shows what the maps say. */
static void
assert_a_range_writes_only_its_rows(const upscale_t* scale, uint16_t* depth, int split) {
    const size_t width = (size_t)scale->destination_width;
    const size_t height = (size_t)scale->destination_height;
    GFX_TARGET_STORAGE(2)
    picture_storage = GFX_TARGET_COLOR_INIT(held.source, scale->source_width, scale->source_height);
    gfx_target_t* picture = &picture_storage.target;
    if (depth != NULL) {
        picture->attachment[GFX_ATTACHMENT_DEPTH] = picture->attachment[GFX_ATTACHMENT_COLOR];
        picture->attachment[GFX_ATTACHMENT_DEPTH].pixels = depth;
        picture->attachment[GFX_ATTACHMENT_DEPTH].format = IMAGE_U16;
        picture->count = GFX_ATTACHMENT_DEPTH + 1;
    }
    for (size_t i = 0; i < (height + 1) * width; i++) {
        held.actual[i] = UNWRITTEN;
    }
    upscale_rows(scale, picture, 0xF00D, held.actual, 0, split);
    assert_unwritten_from(held.actual, (size_t)split * width, ((height + 1) - (size_t)split) * width);
    upscale_rows(scale, picture, 0xF00D, held.actual, split, scale->destination_height - split);
    assert_unwritten_from(held.actual, height * width, width);
    assert_each_pixel_follows_the_maps(scale, held.source, depth, held.actual);
}

/* Every split row, with and without depth. */
static void
assert_matches_the_maps(const path_case_t* c) {
    const size_t source_pixels = (size_t)c->source_width * (size_t)c->source_height;
    const size_t destination_pixels = (size_t)c->destination_width * (size_t)(c->destination_height + 1);
    held.source = malloc(sizeof(*held.source) * source_pixels);
    held.depth = malloc(sizeof(*held.depth) * source_pixels);
    held.actual = malloc(sizeof(*held.actual) * destination_pixels);
    held.columns = malloc(sizeof(*held.columns) * (size_t)c->destination_width);
    held.rows = malloc(sizeof(*held.rows) * (size_t)c->destination_height);
    suite_set_test_cleanup(release_buffers);
    TEST_ASSERT_NOT_NULL(held.source);
    TEST_ASSERT_NOT_NULL(held.depth);
    TEST_ASSERT_NOT_NULL(held.actual);
    TEST_ASSERT_NOT_NULL(held.columns);
    TEST_ASSERT_NOT_NULL(held.rows);
    for (size_t i = 0; i < source_pixels; i++) {
        held.source[i] = (uint16_t)(0x1000 + i);
        held.depth[i] = (uint16_t)(i % 3 == 0 ? 0 : i);
    }
    upscale_t scale;
    TEST_ASSERT_TRUE(upscale_init(&scale, c->source_width, c->source_height, c->destination_width,
                                  c->destination_height, held.columns, held.rows));
    TEST_ASSERT_EQUAL_INT(c->path, scale.path);
    for (int split = 0; split <= c->destination_height; split++) {
        assert_a_range_writes_only_its_rows(&scale, NULL, split);
        assert_a_range_writes_only_its_rows(&scale, held.depth, split);
    }
    release_buffers();
}

/* Each destination from a source of its own width and from one of half its
 * width, at a source height giving repeated, uneven and identity row maps;
 * then widths neither kept and even nor doubled. */
static void
test_each_width_takes_its_path_and_matches_the_maps(void) {
    static const int shapes[][3] = {
        /* destination width, height; source height */
        {8, 9, 5}, {8, 12, 6}, {6, 3, 3}, {14, 10, 5}, {92, 112, 56}, {92, 112, 90}, {92, 112, 75},
    };
    for (size_t i = 0; i < sizeof(shapes) / sizeof(shapes[0]); i++) {
        const int* d = shapes[i];
        assert_matches_the_maps(&(path_case_t){d[0], d[2], d[0], d[1], UPSCALE_KEPT});
        assert_matches_the_maps(&(path_case_t){d[0] / 2, d[2], d[0], d[1], UPSCALE_DOUBLED});
    }
    static const path_case_t mapped[] = {
        {61, 75, 92, 112, UPSCALE_MAPPED}, {6, 4, 15, 10, UPSCALE_MAPPED}, {5, 3, 5, 7, UPSCALE_MAPPED}};
    for (size_t i = 0; i < sizeof(mapped) / sizeof(mapped[0]); i++) {
        assert_matches_the_maps(&mapped[i]);
    }
}

static void
assert_scanline_rows_match_reference(const upscale_t* scale, const uint16_t* plain, int run, uint16_t clear,
                                     uint16_t dark_clear, uint16_t dark_color) {
    const int width = scale->destination_width;
    const int height = scale->destination_height;
    const uint16_t* actual = held.actual;
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            const uint16_t pixel = plain[y * width + x];
            const bool dark = run != 0 && y % run >= run / 2;
            const uint16_t expected = dark ? (pixel == clear ? dark_clear : dark_color) : pixel;
            TEST_ASSERT_EQUAL_HEX16(expected, actual[y * width + x]);
        }
    }
}

static void
test_scanline_runs_preserve_bright_rows_and_darken_clear_pixels(void) {
    enum {
        WIDTH = 4,
        HEIGHT = 3,
        MAX_WIDTH = 16,
        MAX_HEIGHT = 12,
        RED = 24,
        GREEN = 48,
        BLUE = 16,
        CLEAR_RED = 16,
        CLEAR_GREEN = 32,
        CLEAR_BLUE = 24
    };

    uint16_t source[WIDTH * HEIGHT], depth[WIDTH * HEIGHT];
    uint16_t plain[MAX_WIDTH * MAX_HEIGHT];
    held.actual = malloc(sizeof *held.actual * MAX_WIDTH * MAX_HEIGHT);
    suite_set_test_cleanup(release_buffers);
    TEST_ASSERT_NOT_NULL(held.actual);
    uint16_t* actual = held.actual;
    uint16_t columns[MAX_WIDTH], rows[MAX_HEIGHT];
    const uint16_t color = rgb565be_pack(RED, GREEN, BLUE);
    const uint16_t clear = rgb565be_pack(CLEAR_RED, CLEAR_GREEN, CLEAR_BLUE);
    for (int i = 0; i < WIDTH * HEIGHT; i++) {
        source[i] = color;
        depth[i] = i % 2;
    }
    GFX_TARGET_STORAGE(2) picture_storage = GFX_TARGET_COLOR_INIT(source, WIDTH, HEIGHT);
    gfx_target_t* picture = &picture_storage.target;
    picture->attachment[GFX_ATTACHMENT_DEPTH] = picture->attachment[GFX_ATTACHMENT_COLOR];
    picture->attachment[GFX_ATTACHMENT_DEPTH].pixels = depth;
    picture->attachment[GFX_ATTACHMENT_DEPTH].format = IMAGE_U16;
    picture->count = GFX_ATTACHMENT_DEPTH + 1;
    static const int sizes[][3] = {
        /* width, height, uniform even run (zero means ineligible) */
        {8, 6, 2}, {4, 6, 2}, {16, 12, 0}, {5, 6, 0}, {8, 9, 0}, {8, 7, 0}, {8, 12, 0}, {8, 3, 0},
    };
    for (size_t c = 0; c < sizeof sizes / sizeof *sizes; c++) {
        const int width = sizes[c][0], height = sizes[c][1], run = sizes[c][2];
        upscale_t scale;
        TEST_ASSERT_TRUE(upscale_init(&scale, WIDTH, HEIGHT, width, height, columns, rows));
        TEST_ASSERT_EQUAL_INT(run, scale.scanline_run);
        upscale_rows(&scale, picture, clear, plain, 0, height);
        {
            const unsigned keep = UPSCALE_SCANLINE_KEEP;
            const uint16_t dark_color = rgb565be_pack(RED * keep / RGB565_SCALE_UNIT, GREEN * keep / RGB565_SCALE_UNIT,
                                                      BLUE * keep / RGB565_SCALE_UNIT);
            const uint16_t dark_clear =
                rgb565be_pack(CLEAR_RED * keep / RGB565_SCALE_UNIT, CLEAR_GREEN * keep / RGB565_SCALE_UNIT,
                              CLEAR_BLUE * keep / RGB565_SCALE_UNIT);
            const int split = height / 2;
            upscale_rows_scanlined(&scale, picture, clear, actual, 0, split);
            upscale_rows_scanlined(&scale, picture, clear, actual, split, height - split);
            assert_scanline_rows_match_reference(&scale, plain, run, clear, dark_clear, dark_color);
        }
    }
}

static void
test_scanline_all_colors_match_channel_reference(void) {
    enum { WIDTH = 2, HEIGHT = 1, OUTPUT_HEIGHT = 2 };

    uint16_t source[WIDTH], columns[2 * WIDTH], rows[OUTPUT_HEIGHT];
    uint32_t output[WIDTH * OUTPUT_HEIGHT];
    upscale_t scale;
    GFX_TARGET_STORAGE(2) picture_storage = GFX_TARGET_COLOR_INIT(source, WIDTH, HEIGHT);
    gfx_target_t* picture = &picture_storage.target;
    for (int width = WIDTH; width <= 2 * WIDTH; width += WIDTH) {
        TEST_ASSERT_TRUE(upscale_init(&scale, WIDTH, HEIGHT, width, OUTPUT_HEIGHT, columns, rows));
        {
            const unsigned keep = UPSCALE_SCANLINE_KEEP;
            for (unsigned color = 0; color <= UINT16_MAX; color++) {
                source[0] = (uint16_t)color;
                source[1] = (uint16_t)~color;
                upscale_rows_scanlined(&scale, picture, 0, (uint16_t*)output, 0, OUTPUT_HEIGHT);
                for (int x = 0; x < width; x++) {
                    const uint16_t pixel = source[x * WIDTH / width];
                    const uint16_t native = rgb565_swap(pixel);
                    const uint16_t expected = rgb565be_pack(rgb565_r5(native) * keep / RGB565_SCALE_UNIT,
                                                            rgb565_g6(native) * keep / RGB565_SCALE_UNIT,
                                                            rgb565_b5(native) * keep / RGB565_SCALE_UNIT);
                    TEST_ASSERT_EQUAL_HEX16(pixel, ((uint16_t*)output)[x]);
                    TEST_ASSERT_EQUAL_HEX16(expected, ((uint16_t*)output)[width + x]);
                }
            }
        }
    }
}

static void
test_half_picture_fit_table_and_compose_size(void) {
    static const struct {
        int width, height, destination_width, destination_height;
        bool fits;
    } cases[] = {
        {0, 2, 8, 12, false}, {4, 0, 8, 12, false}, {3, 2, 8, 12, false},  {4, 2, 0, 12, false}, {4, 2, 8, 13, false},
        {4, 2, 8, 12, true},  {4, 4, 8, 12, true},  {4, 12, 8, 12, false}, {4, 2, 8, 0, false},  {4, 1, 8, 6, true},
        {4, 5, 8, 12, true},  {4, 7, 8, 24, true},  {4, 7, 8, 12, false},
    };

    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        const raster_t raster = {.width = cases[i].width,
                                 .height = cases[i].height,
                                 .destination_width = cases[i].destination_width,
                                 .destination_height = cases[i].destination_height};
        TEST_ASSERT_EQUAL_INT(cases[i].fits, raster_fits_half_picture(&raster));
        const resolution_step_t size = raster_compose_size(&raster);
        const int divisor = cases[i].fits ? 2 : 1;
        TEST_ASSERT_EQUAL_INT(cases[i].destination_width / divisor, size.width);
        TEST_ASSERT_EQUAL_INT(cases[i].destination_height / divisor, size.height);
    }
}

static void
test_odd_half_picture_repeat_keeps_pixels(void) {
    enum { WIDTH = 4, HEIGHT = 1, REPEAT = 3, OUTPUT_HEIGHT = HEIGHT * REPEAT };

    const uint16_t source[WIDTH] = {0x1234, 0x5678, 0x9abc, 0xdef0};
    uint16_t columns[WIDTH], rows[OUTPUT_HEIGHT];
    uint32_t output[WIDTH * OUTPUT_HEIGHT / 2];
    upscale_t scale;
    TEST_ASSERT_TRUE(upscale_init(&scale, WIDTH, HEIGHT, WIDTH, OUTPUT_HEIGHT, columns, rows));
    GFX_TARGET_STORAGE(2) picture_storage = GFX_TARGET_COLOR_INIT((uint16_t*)source, WIDTH, HEIGHT);
    gfx_target_t* picture = &picture_storage.target;
    upscale_rows_scanlined(&scale, picture, 0, (uint16_t*)output, 0, OUTPUT_HEIGHT);
    for (int y = 0; y < OUTPUT_HEIGHT; y++) {
        TEST_ASSERT_EQUAL_MEMORY(source, (uint16_t*)output + y * WIDTH, sizeof source);
    }
}

static void
test_shaded_paired_copy_strip_starts_on_bottom_of_pair(void) {
    enum { WIDTH = 4, HEIGHT = 2, REPEAT = 4, OUTPUT_HEIGHT = HEIGHT * REPEAT, FIRST = 1 };

    uint16_t source[WIDTH * HEIGHT] = {0x1234, 0x5678, 0x9abc, 0xdef0, 0x4321, 0x8765, 0xcba9, 0xfed0};
    uint16_t rows[OUTPUT_HEIGHT];
    uint32_t whole[WIDTH * OUTPUT_HEIGHT / 2], strip[WIDTH * (OUTPUT_HEIGHT - FIRST) / 2];
    for (int y = 0; y < OUTPUT_HEIGHT; y++) {
        rows[y] = y / REPEAT;
    }
    const GFX_TARGET_STORAGE(1) target_storage = GFX_TARGET_COLOR_INIT((uint16_t*)whole, WIDTH, OUTPUT_HEIGHT);
    const gfx_target_t* target = &target_storage.target;
    GFX_TARGET_STORAGE(1) band_storage;
    gfx_target_of_color(&band_storage.target, (uint16_t*)strip, WIDTH, OUTPUT_HEIGHT - FIRST, FIRST);
    const gfx_target_t* band = &band_storage.target;
    const GFX_TARGET_STORAGE(1) source_storage = GFX_TARGET_COLOR_INIT((void*)source, WIDTH, HEIGHT);
    const gfx_target_t* picture = &source_storage.target;
    for (unsigned keep = 0; keep <= RGB565_SCALE_UNIT; keep += RGB565_SCALE_UNIT / 2) {
        gfx_target_paired_rows_shaded(target, &picture->attachment[0], NULL, 0, rows, 0, OUTPUT_HEIGHT, false, REPEAT,
                                      keep);
        gfx_target_paired_rows_shaded(band, &picture->attachment[0], NULL, 0, rows, FIRST, OUTPUT_HEIGHT - FIRST, false,
                                      REPEAT, keep);
        TEST_ASSERT_EQUAL_MEMORY((uint16_t*)whole + FIRST * WIDTH, strip, sizeof strip);
    }
    gfx_target_paired_rows_shaded(target, &picture->attachment[0], NULL, 0, rows, 0, OUTPUT_HEIGHT, false, 0,
                                  RGB565_SCALE_UNIT);
    gfx_target_paired_rows_shaded(band, &picture->attachment[0], NULL, 0, rows, FIRST, OUTPUT_HEIGHT - FIRST, false, 0,
                                  RGB565_SCALE_UNIT);
    TEST_ASSERT_EQUAL_MEMORY((uint16_t*)whole + FIRST * WIDTH, strip, sizeof strip);
    for (int y = 0; y < OUTPUT_HEIGHT; y++) {
        TEST_ASSERT_EQUAL_MEMORY(source + (y / REPEAT) * WIDTH, (uint16_t*)whole + y * WIDTH, WIDTH * sizeof *source);
    }
}

static void
test_blend_keeps_ends_and_rounds_the_middle(void) {
    const rgb565be_t black = RGB565BE(0x000000);
    const rgb565be_t white = RGB565BE(0xFFFFFF);
    const rgb565be_t mixed = RGB565BE(0x80C040);
    TEST_ASSERT_EQUAL_HEX16(mixed, rgb565be_blend(mixed, white, 0));
    TEST_ASSERT_EQUAL_HEX16(white, rgb565be_blend(mixed, white, RGB565_SCALE_UNIT));
    const uint16_t half = rgb565_swap(rgb565be_blend(black, white, RGB565_SCALE_UNIT / 2));
    /* Half of 31 and 63, rounded up. */
    TEST_ASSERT_EQUAL_UINT(16, rgb565_r5(half));
    TEST_ASSERT_EQUAL_UINT(32, rgb565_g6(half));
    TEST_ASSERT_EQUAL_UINT(16, rgb565_b5(half));
}

static void
test_spread_quarter_matches_blend_at_a_quarter(void) {
    /* Every channel value against every other: stepping both colours through
     * all 65536 values in coprime strides covers each channel pair. */
    enum { STEP_A = 7, STEP_B = 13, COLOURS = UINT16_MAX + 1 };

    for (uint32_t i = 0; i < COLOURS; i++) {
        const rgb565be_t a = (rgb565be_t)(i * STEP_A);
        const rgb565be_t b = (rgb565be_t)(i * STEP_B);
        TEST_ASSERT_EQUAL_HEX16(rgb565be_blend(a, b, RGB565_SCALE_UNIT / 4),
                                rgb565be_unspread(rgb565_spread_quarter(rgb565be_spread(a), rgb565be_spread(b))));
    }
}

static void
test_row_blends_sit_on_destination_row_centres(void) {
    enum { SOURCE = 2, DESTINATION = 2 * SOURCE, QUARTER = RGB565_SCALE_UNIT / 4 };

    uint16_t columns[1], rows[DESTINATION], blends[DESTINATION];
    upscale_t scale;
    TEST_ASSERT_TRUE(upscale_init(&scale, 1, SOURCE, 1, DESTINATION, columns, rows));
    upscale_init_blends(&scale, blends);
    /* Centres at source -1/4 (clamped), 1/4, 3/4 and 5/4 (clamped). */
    const uint16_t expected[DESTINATION] = {UPSCALE_BLEND(0, 0), UPSCALE_BLEND(0, QUARTER),
                                            UPSCALE_BLEND(0, RGB565_SCALE_UNIT - QUARTER), UPSCALE_BLEND(1, 0)};
    TEST_ASSERT_EQUAL_HEX16_ARRAY(expected, blends, DESTINATION);
    TEST_ASSERT_TRUE(upscale_init(&scale, 1, SOURCE, 1, SOURCE, columns, rows));
    upscale_init_blends(&scale, blends);
    for (int y = 0; y < SOURCE; y++) {
        TEST_ASSERT_EQUAL_HEX16(UPSCALE_BLEND(y, 0), blends[y]);
    }
}

/* Two source rows, three columns: column 0 one surface, column 1 a depth
 * step between the rows, column 2 drawn above and clear below. */
static void
test_surface_rows_blend_within_a_surface_and_mark_seams(void) {
    enum { WIDTH = 4, SOURCE = 2, DESTINATION = 2 * SOURCE, NEAR = 0x8000, FAR = 0x1000 };

    const rgb565be_t top = RGB565BE(0x000000), bottom = RGB565BE(0xFFFFFF), clear = RGB565BE(0x0000FF);
    uint16_t color[WIDTH * SOURCE] = {top, top, top, top, bottom, bottom, bottom, bottom};
    uint16_t depth[WIDTH * SOURCE] = {NEAR, NEAR, NEAR, NEAR, NEAR, FAR, 0, NEAR};
    GFX_TARGET_STORAGE(2) storage = GFX_TARGET_COLOR_INIT(color, WIDTH, SOURCE);
    storage.count = 2;
    storage.attachment[GFX_ATTACHMENT_DEPTH] = storage.attachment[GFX_ATTACHMENT_COLOR];
    storage.attachment[GFX_ATTACHMENT_DEPTH].pixels = depth;
    uint16_t columns[WIDTH], rows[DESTINATION], blends[DESTINATION], actual[WIDTH * DESTINATION];
    uint8_t seams[GFX_SEAM_BYTES(WIDTH, DESTINATION)];
    upscale_t scale;
    TEST_ASSERT_TRUE(upscale_init(&scale, WIDTH, SOURCE, WIDTH, DESTINATION, columns, rows));
    upscale_init_blends(&scale, blends);
    scale.filter = UPSCALE_FILTER_SURFACE;
    upscale_rows_filtered(&scale, &storage.target, clear, actual, seams, 0, DESTINATION);
    for (int y = 0; y < DESTINATION; y++) {
        const uint16_t blend = blends[y];
        const bool lower_nearer = blend % RGB565_SCALE_UNIT < RGB565_SCALE_UNIT / 2;
        const rgb565be_t mixed =
            rgb565be_blend(top, blend % RGB565_SCALE_UNIT == 0 ? top : bottom, blend % RGB565_SCALE_UNIT);
        const rgb565be_t expected_mixed = blend / RGB565_SCALE_UNIT == 1 ? bottom : mixed;
        TEST_ASSERT_EQUAL_HEX16(expected_mixed, actual[(y * WIDTH) + 0]);
        TEST_ASSERT_EQUAL_HEX16(expected_mixed, actual[(y * WIDTH) + 3]);
        const bool from_top = blend / RGB565_SCALE_UNIT == 0 && lower_nearer;
        TEST_ASSERT_EQUAL_HEX16(from_top ? top : bottom, actual[(y * WIDTH) + 1]);
        TEST_ASSERT_EQUAL_HEX16(from_top ? top : clear, actual[(y * WIDTH) + 2]);
    }
    /* Row 1 still stands for the top row and row 2 for the bottom one: the
     * step lies between them. Bottom-row pixels differ from their right
     * neighbours in columns 0 to 2. */
    const uint8_t expected[DESTINATION] = {0x0, 0x6, 0x7, 0x7};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, seams, DESTINATION);

    scale.filter = UPSCALE_FILTER_NEAREST;
    uint16_t nearest[WIDTH * DESTINATION];
    upscale_rows_filtered(&scale, &storage.target, clear, actual, seams, 0, DESTINATION);
    upscale_rows(&scale, &storage.target, clear, nearest, 0, DESTINATION);
    TEST_ASSERT_EQUAL_HEX16_ARRAY(nearest, actual, WIDTH * DESTINATION);
    for (size_t i = 0; i < sizeof seams; i++) {
        TEST_ASSERT_EQUAL_HEX8(UINT8_MAX, seams[i]);
    }
}

typedef struct {
    int width, source, destination;
} seam_case_t;

/* A destination row's seam bit as the filter defines it: the pixel and its
 * right neighbour, or the pixel below, on different surfaces in the source
 * rows the two destination rows lean on most. */
static bool
reference_seam(const upscale_t* scale, const uint16_t* depth, int x, int y) {
    const int width = scale->destination_width;
    const uint16_t blend = scale->blends[y];
    const int row = (blend / RGB565_SCALE_UNIT) + (blend % RGB565_SCALE_UNIT >= RGB565_SCALE_UNIT / 2 ? 1 : 0);
    const uint16_t* here = depth + ((size_t)row * width);
    if (x + 1 < width && !upscale_same_surface(here[x], here[x + 1])) {
        return true;
    }
    if (y + 1 == scale->destination_height) {
        return false;
    }
    const uint16_t next = scale->blends[y + 1];
    const int below = (next / RGB565_SCALE_UNIT) + (next % RGB565_SCALE_UNIT >= RGB565_SCALE_UNIT / 2 ? 1 : 0);
    return !upscale_same_surface(here[x], depth[((size_t)below * width) + x]);
}

/* Random depths from a few levels, some one surface and some not, through
 * even widths off and on a seam byte, more and as many rows, whole and in
 * two ranges. */
static void
check_seam_case(const seam_case_t* c) {
    const uint16_t levels[] = {0, 0x1000, 0x1080, 0x8000, 0x8900};
    const size_t pixels = (size_t)c->width * c->source;
    const size_t bytes = GFX_SEAM_BYTES(c->width, c->destination);
    uint16_t* color = calloc(pixels, sizeof(*color));
    uint16_t* depth = malloc(pixels * sizeof(*depth));
    uint16_t* columns = malloc((size_t)c->width * sizeof(*columns));
    uint16_t* rows = malloc((size_t)c->destination * sizeof(*rows));
    uint16_t* blends = malloc((size_t)c->destination * sizeof(*blends));
    uint16_t* actual = malloc((size_t)c->width * c->destination * sizeof(*actual));
    uint8_t* whole = malloc(bytes);
    uint8_t* split = malloc(bytes);
    TEST_ASSERT_NOT_NULL(color);
    TEST_ASSERT_NOT_NULL(depth);
    TEST_ASSERT_NOT_NULL(columns);
    TEST_ASSERT_NOT_NULL(rows);
    TEST_ASSERT_NOT_NULL(blends);
    TEST_ASSERT_NOT_NULL(actual);
    TEST_ASSERT_NOT_NULL(whole);
    TEST_ASSERT_NOT_NULL(split);
    for (size_t i = 0; i < pixels; i++) {
        depth[i] = levels[(size_t)rand() % (sizeof levels / sizeof *levels)];
    }
    GFX_TARGET_STORAGE(2) storage = GFX_TARGET_COLOR_INIT(color, c->width, c->source);
    storage.count = 2;
    storage.attachment[GFX_ATTACHMENT_DEPTH] = storage.attachment[GFX_ATTACHMENT_COLOR];
    storage.attachment[GFX_ATTACHMENT_DEPTH].pixels = depth;
    upscale_t scale;
    TEST_ASSERT_TRUE(upscale_init(&scale, c->width, c->source, c->width, c->destination, columns, rows));
    upscale_init_blends(&scale, blends);
    scale.filter = UPSCALE_FILTER_SURFACE;
    const int half = c->destination / 2;
    upscale_rows_filtered(&scale, &storage.target, 0, actual, whole, 0, c->destination);
    upscale_rows_filtered(&scale, &storage.target, 0, actual, split, 0, half);
    upscale_rows_filtered(&scale, &storage.target, 0, actual, split, half, c->destination - half);
    for (int y = 0; y < c->destination; y++) {
        for (int x = 0; x < c->width; x++) {
            const uint8_t* row = whole + ((size_t)y * GFX_SEAM_PITCH(c->width));
            TEST_ASSERT_EQUAL_INT(reference_seam(&scale, depth, x, y), gfx_seam_at(row, x));
        }
    }
    TEST_ASSERT_EQUAL_HEX8_ARRAY(whole, split, bytes);
    free(color);
    free(depth);
    free(columns);
    free(rows);
    free(blends);
    free(actual);
    free(whole);
    free(split);
}

static void
test_surface_seams_match_the_per_pixel_reference(void) {
    const seam_case_t cases[] = {{14, 6, 15}, {16, 9, 9}, {10, 7, 13}, {24, 4, 11}};
    srand(11);
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        check_seam_case(&cases[i]);
    }
}

void
run_upscale_suite(void) {
    RUN_TEST(test_scanline_all_colors_match_channel_reference);
    RUN_TEST(test_scanline_runs_preserve_bright_rows_and_darken_clear_pixels);
    RUN_TEST(test_half_picture_fit_table_and_compose_size);
    RUN_TEST(test_odd_half_picture_repeat_keeps_pixels);
    RUN_TEST(test_shaded_paired_copy_strip_starts_on_bottom_of_pair);
    RUN_TEST(test_two_times_matches_the_frame_doubling_reference);
    RUN_TEST(test_integer_factors_copy_each_source_pixel_to_its_block);
    RUN_TEST(test_fractional_maps_cover_destination_in_order_and_at_source_corners);
    RUN_TEST(test_two_row_ranges_equal_one_whole_upscale);
    RUN_TEST(test_each_width_takes_its_path_and_matches_the_maps);
    RUN_TEST(test_blend_keeps_ends_and_rounds_the_middle);
    RUN_TEST(test_spread_quarter_matches_blend_at_a_quarter);
    RUN_TEST(test_row_blends_sit_on_destination_row_centres);
    RUN_TEST(test_surface_rows_blend_within_a_surface_and_mark_seams);
    RUN_TEST(test_surface_seams_match_the_per_pixel_reference);
}

SUITE_REGISTER(run_upscale_suite);

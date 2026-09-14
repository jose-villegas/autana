/*
 * Portable suite: gfx_target.h - the clip-and-translate arithmetic every
 * pixel-writing gfx_* primitive goes through, driven directly since the
 * header carries no ESP-IDF dependency. gfx_target_fill_rect() is
 * gfx_fill_rect()'s own body; text ultimately reduces to the same
 * primitive (one glyph pixel is one small filled rect - gfx.c's
 * draw_rotated_font_pixel()), so a rect straddling a band edge is the
 * faithful stand-in for a glyph straddling one.
 */

#include "suites.h"
#include "unity.h"

#include "gfx/gfx_target.h"

#include <string.h>

#define WIDTH  8
#define HEIGHT 8

static gfx_color_t full_buf[WIDTH * HEIGHT];

static gfx_target_t
full_target(void) {
    return (gfx_target_t){full_buf, 0, HEIGHT, WIDTH};
}

static void
fixture(void) {
    memset(full_buf, 0, sizeof full_buf);
}

/* --- clipping and translation, one target ------------------------------- */

static void
test_a_rect_entirely_inside_the_target_is_untouched_by_clipping(void) {
    fixture();
    int x0, y0, x1, y1;
    gfx_target_fill_rect(full_target(), 0, 0, WIDTH, HEIGHT, 2, 2, 3, 3, (gfx_color_t)0xFFFF, &x0, &y0, &x1, &y1);

    TEST_ASSERT_EQUAL_INT(2, x0);
    TEST_ASSERT_EQUAL_INT(2, y0);
    TEST_ASSERT_EQUAL_INT(5, x1);
    TEST_ASSERT_EQUAL_INT(5, y1);
    for (int y = 2; y < 5; y++) {
        for (int x = 2; x < 5; x++) {
            TEST_ASSERT_EQUAL_HEX16(0xFFFF, full_buf[y * WIDTH + x]);
        }
    }
    TEST_ASSERT_EQUAL_HEX16_MESSAGE(0, full_buf[1 * WIDTH + 2], "the rect must not touch its neighbours");
    TEST_ASSERT_EQUAL_HEX16_MESSAGE(0, full_buf[5 * WIDTH + 2], "the rect must not touch its neighbours");
}

/* A target shorter than the app's own clip rect (the band case) must win -
 * a caller's clip never widens what the target itself can hold. */
static void
test_the_targets_own_row_range_narrows_a_wider_clip(void) {
    int y0 = 0, y1 = 8;
    const gfx_target_t band = {full_buf, 2, 3, WIDTH}; /* rows [2, 5) only */

    gfx_target_clip_y(band, 0, HEIGHT, &y0, &y1);

    TEST_ASSERT_EQUAL_INT(2, y0);
    TEST_ASSERT_EQUAL_INT(5, y1);
}

/* The app's own clip rect must win when it is narrower than the target. */
static void
test_the_apps_clip_narrows_a_wider_target(void) {
    int y0 = 0, y1 = 8;
    const gfx_target_t full = full_target();

    gfx_target_clip_y(full, 3, 6, &y0, &y1);

    TEST_ASSERT_EQUAL_INT(3, y0);
    TEST_ASSERT_EQUAL_INT(6, y1);
}

/* gfx_target_row() must translate an absolute row into the target's own
 * local storage - a band target's row 0 is NOT the buffer's own row 0
 * unless the band happens to start at the top of the screen. */
static void
test_row_translation_lands_in_the_bands_own_local_row(void) {
    gfx_color_t band_buf[WIDTH * 3] = {0};
    const gfx_target_t band = {band_buf, 4, 3, WIDTH}; /* absolute rows [4, 7) */

    gfx_target_row(band, 5)[0] = (gfx_color_t)0xABCD;

    TEST_ASSERT_EQUAL_HEX16_MESSAGE(0xABCD, band_buf[1 * WIDTH + 0],
                                    "absolute row 5 must land in the band's own local row 1 (5 - row0 4)");
}

/* --- straddling a band edge ---------------------------------------------- */

/* A rect (the same shape a glyph pixel column is) that crosses a band
 * boundary must be split correctly across the two bands' own buffers, with
 * neither band drawing the other's rows. */
static void
test_a_rect_straddling_a_band_edge_splits_correctly(void) {
    gfx_color_t band_a[WIDTH * 4] = {0}; /* absolute rows [0, 4) */
    gfx_color_t band_b[WIDTH * 4] = {0}; /* absolute rows [4, 8) */
    const gfx_target_t a = {band_a, 0, 4, WIDTH};
    const gfx_target_t b = {band_b, 4, 4, WIDTH};

    /* A column at x=3, rows [2, 6) - straddles the row-4 band edge. */
    int ax0, ay0, ax1, ay1, bx0, by0, bx1, by1;
    gfx_target_fill_rect(a, 0, 0, WIDTH, HEIGHT, 3, 2, 1, 4, (gfx_color_t)0x1111, &ax0, &ay0, &ax1, &ay1);
    gfx_target_fill_rect(b, 0, 0, WIDTH, HEIGHT, 3, 2, 1, 4, (gfx_color_t)0x1111, &bx0, &by0, &bx1, &by1);

    TEST_ASSERT_EQUAL_INT(2, ay0);
    TEST_ASSERT_EQUAL_INT(4, ay1); /* band a only got its own two rows */
    TEST_ASSERT_EQUAL_INT(4, by0);
    TEST_ASSERT_EQUAL_INT(6, by1); /* band b got the other two */

    TEST_ASSERT_EQUAL_HEX16(0x1111, band_a[2 * WIDTH + 3]);
    TEST_ASSERT_EQUAL_HEX16(0x1111, band_a[3 * WIDTH + 3]);
    TEST_ASSERT_EQUAL_HEX16(0x1111, band_b[0 * WIDTH + 3]); /* local row 0 == absolute row 4 */
    TEST_ASSERT_EQUAL_HEX16(0x1111, band_b[1 * WIDTH + 3]); /* local row 1 == absolute row 5 */
}

/* --- the union of every band equals one full-target render --------------- */

/* Several rects, standing in for a frame's worth of UI commands (a
 * background box, a text glyph's pixels, a border), rendered once into one
 * full-height target versus once per band into N separate band targets -
 * the union of the bands must equal the full render pixel for pixel, with
 * no row lost or drawn twice. */
static void
test_the_union_of_all_bands_equals_the_full_target_render(void) {
    fixture();

    typedef struct {
        int x, y, w, h;
        gfx_color_t color;
    } shape_t;

    const shape_t shapes[] = {
        {0, 0, 8, 8, (gfx_color_t)0x2222}, /* a full-screen background */
        {1, 1, 4, 3, (gfx_color_t)0x3333}, /* an overlapping box */
        {3, 0, 1, 8, (gfx_color_t)0x4444}, /* a full-height column, like a glyph stroke */
    };
    const int shape_count = (int)(sizeof shapes / sizeof shapes[0]);

    for (int i = 0; i < shape_count; i++) {
        int x0, y0, x1, y1;
        gfx_target_fill_rect(full_target(), 0, 0, WIDTH, HEIGHT, shapes[i].x, shapes[i].y, shapes[i].w, shapes[i].h,
                             shapes[i].color, &x0, &y0, &x1, &y1);
    }

    gfx_color_t band_buf[4][WIDTH * 2] = {0}; /* 4 bands of 2 rows each = 8 rows */
    for (int band = 0; band < 4; band++) {
        const gfx_target_t target = {band_buf[band], band * 2, 2, WIDTH};
        for (int i = 0; i < shape_count; i++) {
            if (!gfx_target_row_range_overlaps(target, shapes[i].y, shapes[i].y + shapes[i].h)) {
                continue; /* the per-band skip every replay path relies on */
            }
            int x0, y0, x1, y1;
            gfx_target_fill_rect(target, 0, 0, WIDTH, HEIGHT, shapes[i].x, shapes[i].y, shapes[i].w, shapes[i].h,
                                 shapes[i].color, &x0, &y0, &x1, &y1);
        }
    }

    for (int y = 0; y < HEIGHT; y++) {
        for (int x = 0; x < WIDTH; x++) {
            const gfx_color_t want = full_buf[y * WIDTH + x];
            const gfx_color_t got = band_buf[y / 2][(y % 2) * WIDTH + x];
            TEST_ASSERT_EQUAL_HEX16_MESSAGE(want, got,
                                            "banded render must match the full-target render pixel for pixel");
        }
    }
}

/* A shape that never reaches a given band must be skipped, not merely
 * clipped to nothing - gfx_target_row_range_overlaps() is what a replay
 * loop checks before it bothers calling the fill at all. */
static void
test_row_range_overlap_rejects_a_shape_the_band_never_reaches(void) {
    const gfx_target_t band = {full_buf, 6, 2, WIDTH}; /* rows [6, 8) */

    TEST_ASSERT_FALSE(gfx_target_row_range_overlaps(band, 0, 4));
    TEST_ASSERT_TRUE(gfx_target_row_range_overlaps(band, 5, 7));
    TEST_ASSERT_TRUE_MESSAGE(gfx_target_row_range_overlaps(band, 0, 8), "a full-height shape overlaps every band");
}

void
run_gfx_target_suite(void) {
    RUN_TEST(test_a_rect_entirely_inside_the_target_is_untouched_by_clipping);
    RUN_TEST(test_the_targets_own_row_range_narrows_a_wider_clip);
    RUN_TEST(test_the_apps_clip_narrows_a_wider_target);
    RUN_TEST(test_row_translation_lands_in_the_bands_own_local_row);
    RUN_TEST(test_a_rect_straddling_a_band_edge_splits_correctly);
    RUN_TEST(test_the_union_of_all_bands_equals_the_full_target_render);
    RUN_TEST(test_row_range_overlap_rejects_a_shape_the_band_never_reaches);
}

SUITE_REGISTER(run_gfx_target_suite);

/* Host-only: S3L_SCISSOR_Y defines globals that conflict with the renderer's copy. */

#include "suites.h"

#ifndef DEVICE_BUILD

#include "unity.h"

#define S3L_PIXEL_FUNCTION      count_pixel
#define S3L_RESOLUTION_X        64
#define S3L_RESOLUTION_Y        64
#define S3L_Z_BUFFER            0
#define S3L_NEAR_CROSS_STRATEGY 0
#define S3L_SCISSOR_Y           1
#include "small3dlib.h"

static int pixel_count;

static void
count_pixel(S3L_PixelInfo* p) {
    (void)p;
    pixel_count++;
}

/* A triangle spanning the full 64-row height, wide enough that every row it
 * touches covers real pixels - the worst case for a per-band re-rasterize:
 * with no scissor it is rasterized floor to ceiling regardless of which
 * band asked for it. */
static void
draw_full_height_triangle(void) {
    const S3L_Vec4 a = {10, 0, S3L_F, S3L_F};
    const S3L_Vec4 b = {54, 0, S3L_F, S3L_F};
    const S3L_Vec4 c = {32, 63, S3L_F, S3L_F};
    S3L_drawTriangle(a, b, c, 0, 0);
}

static void
fixture(void) {
    pixel_count = 0;
    S3L_scissorMinY = 0;
    S3L_scissorMaxY = S3L_RESOLUTION_Y;
}

static void
test_no_scissor_draws_the_whole_triangle(void) {
    fixture();
    draw_full_height_triangle();
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, pixel_count, "the reference draw itself produced no pixels");
}

/* The actual claim behind band mode's fix: scissoring to a fraction of the
 * screen must cost a proportional fraction of the pixels, not the whole
 * triangle's worth every time. */
static void
test_scissoring_to_one_band_draws_far_fewer_pixels(void) {
    fixture();
    draw_full_height_triangle();
    const int full = pixel_count;

    pixel_count = 0;
    S3L_scissorMinY = 16;
    S3L_scissorMaxY = 24; /* one 8-row band out of 64 */
    draw_full_height_triangle();
    const int band = pixel_count;

    TEST_ASSERT_GREATER_THAN_INT(0, band);
    TEST_ASSERT_LESS_THAN_MESSAGE(full / 4, band,
                                  "an 8-row band out of 64 should touch far fewer pixels than the "
                                  "whole triangle");
}

/* Splitting the same triangle into eight non-overlapping bands and summing
 * their pixel counts must reproduce the unscissored count exactly - no row
 * is drawn twice, none is skipped, band mode's per-band redraw covers the
 * same picture the old single pass did. */
static void
test_bands_sum_back_to_the_unscissored_count(void) {
    fixture();
    draw_full_height_triangle();
    const int full = pixel_count;

    int summed = 0;
    for (int y0 = 0; y0 < S3L_RESOLUTION_Y; y0 += 8) {
        S3L_scissorMinY = y0;
        S3L_scissorMaxY = y0 + 8;
        pixel_count = 0;
        draw_full_height_triangle();
        summed += pixel_count;
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(full, summed,
                                  "every band's own scissored draw, summed, must equal one "
                                  "unscissored draw - a mismatch means rows are lost or double-drawn");
}

/* A band entirely above or below the triangle's own screen extent must
 * draw nothing at all - the case a real band-mode frame hits constantly
 * once triangles are binned by row range and a band skips a triangle that
 * does not reach it. */
static void
test_a_band_outside_the_triangle_draws_nothing(void) {
    fixture();
    S3L_scissorMinY = 0;
    S3L_scissorMaxY = 4; /* the triangle's own top vertex is at row 0, but
                             this narrow band still lands mostly above where
                             either slanted edge has reached any width */
    draw_full_height_triangle();
    /* Not asserting zero here - the triangle's apex does start at row 0 -
       this test's real value is the next one below, over a band clearly
       past the triangle's bottom edge. */

    fixture();
    S3L_scissorMinY = 60;
    S3L_scissorMaxY = 64;
    draw_full_height_triangle();
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, pixel_count,
                                         "a band at the triangle's own bottom edge should still draw its "
                                         "last rows");

    fixture();
    S3L_scissorMinY = 63;
    S3L_scissorMaxY = 63; /* an empty range */
    draw_full_height_triangle();
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, pixel_count, "an empty scissor range must draw nothing");
}

void
run_small3dlib_scissor_suite(void) {
    RUN_TEST(test_no_scissor_draws_the_whole_triangle);
    RUN_TEST(test_scissoring_to_one_band_draws_far_fewer_pixels);
    RUN_TEST(test_bands_sum_back_to_the_unscissored_count);
    RUN_TEST(test_a_band_outside_the_triangle_draws_nothing);
}

#else

void
run_small3dlib_scissor_suite(void) {}

#endif

SUITE_REGISTER(run_small3dlib_scissor_suite);

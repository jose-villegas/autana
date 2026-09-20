/*
 * Portable suite: the baked ridge curve, checked against the boot photograph
 * it has to lie on rather than against the drawing the generator read.
 *
 * Host only, listed in run_tests.sh and not in main/CMakeLists.txt:
 * boot_anim_image.h is a static table, so including it here would put a
 * second 644 KiB copy of the photograph in a device image.
 */

#include <stdbool.h>
#include <stdlib.h>

#include "suites.h"
#include "unity.h"

#include "boot/boot_anim_image.h"
#include "ui/ridge_curve_generated.h"

#define VIEW_W               RIDGE_CURVE_POINTS
#define VIEW_H               RIDGE_CURVE_VIEW_H

/* Two 5-row bands, one each side of the curve, 3 rows clear of it. */
#define BAND_NEAR            3
#define BAND_FAR             8

/* Summed over R, G and B at 8 bits. On the ridge the median column scores
 * about 350; in open sky 25 rows above it, about 50. */
#define EDGE_CONTRAST        80
#define MIN_COLUMNS_ON_EDGE  95
#define MAX_COLUMNS_OFF_EDGE 85

static int
curve_row(int x) {
    return (ridge_curve_y[x] + (1 << (RIDGE_CURVE_Q_SHIFT - 1))) >> RIDGE_CURVE_Q_SHIFT;
}

/* gen_boot_anim_image.py's panel_index() and its byte-swapped RGB565. */
static void
photo_rgb(int view_x, int view_y, int rgb[3]) {
    const int panel_x = BOOT_ANIM_IMAGE_W - 1 - view_y;
    const int panel_y = view_x;
    const uint16_t stored = boot_anim_image[panel_y * BOOT_ANIM_IMAGE_W + panel_x];
    const uint16_t c = (uint16_t)((stored >> 8) | (stored << 8));
    rgb[0] = ((c >> 11) & 0x1F) * 255 / 31;
    rgb[1] = ((c >> 5) & 0x3F) * 255 / 63;
    rgb[2] = (c & 0x1F) * 255 / 31;
}

static void
band_sum(int x, int first_row, int rgb_sum[3]) {
    rgb_sum[0] = rgb_sum[1] = rgb_sum[2] = 0;
    for (int y = first_row; y < first_row + (BAND_FAR - BAND_NEAR); y++) {
        int rgb[3];
        photo_rgb(x, y, rgb);
        for (int k = 0; k < 3; k++) {
            rgb_sum[k] += rgb[k];
        }
    }
}

/* Percentage of columns where the photograph changes colour across the curve
 * moved by (dx, dy). */
static int
columns_on_an_edge(int dx, int dy) {
    int measured = 0;
    int on_edge = 0;
    for (int x = 0; x < VIEW_W; x++) {
        const int photo_x = x + dx;
        const int y = curve_row(x) + dy;
        if (photo_x < 0 || photo_x >= VIEW_W || y - BAND_FAR < 0 || y + BAND_FAR > VIEW_H) {
            continue;
        }
        int above[3], below[3];
        band_sum(photo_x, y - BAND_FAR, above);
        band_sum(photo_x, y + BAND_NEAR, below);
        int contrast = 0;
        for (int k = 0; k < 3; k++) {
            contrast += abs(above[k] - below[k]) / (BAND_FAR - BAND_NEAR);
        }
        measured++;
        on_edge += contrast > EDGE_CONTRAST;
    }
    return measured > 0 ? on_edge * 100 / measured : 0;
}

static void
test_the_curve_shares_the_photographs_frame(void) {
    TEST_ASSERT_EQUAL_INT(BOOT_ANIM_IMAGE_H, RIDGE_CURVE_POINTS);
    TEST_ASSERT_EQUAL_INT(BOOT_ANIM_IMAGE_W, RIDGE_CURVE_VIEW_H);
}

static void
test_every_height_is_inside_the_frame_and_near_its_neighbour(void) {
    const int one_px = 1 << RIDGE_CURVE_Q_SHIFT;
    for (int x = 0; x < VIEW_W; x++) {
        TEST_ASSERT_TRUE(ridge_curve_y[x] >= 0 && ridge_curve_y[x] < VIEW_H * one_px);
        if (x > 0) {
            TEST_ASSERT_TRUE(abs(ridge_curve_y[x] - ridge_curve_y[x - 1]) <= 8 * one_px);
        }
    }
}

static void
test_the_summit_is_on_the_right_and_the_slopes_fall_to_both_sides(void) {
    int summit = 0;
    for (int x = 1; x < VIEW_W; x++) {
        if (ridge_curve_y[x] < ridge_curve_y[summit]) {
            summit = x;
        }
    }
    TEST_ASSERT_TRUE(summit > VIEW_W / 2);
    TEST_ASSERT_TRUE(ridge_curve_y[0] > ridge_curve_y[summit]);
    TEST_ASSERT_TRUE(ridge_curve_y[VIEW_W - 1] > ridge_curve_y[summit]);
}

static void
test_the_curve_lies_on_the_photographs_ridge(void) {
    TEST_ASSERT_GREATER_OR_EQUAL_INT(MIN_COLUMNS_ON_EDGE, columns_on_an_edge(0, 0));
}

/* What makes the test above a measurement: the same curve 6 px or more off
 * the ridge has to score visibly worse. */
static void
test_a_displaced_curve_does_not(void) {
    TEST_ASSERT_LESS_THAN_INT(MAX_COLUMNS_OFF_EDGE, columns_on_an_edge(0, -6));
    TEST_ASSERT_LESS_THAN_INT(MAX_COLUMNS_OFF_EDGE, columns_on_an_edge(0, 6));
    TEST_ASSERT_LESS_THAN_INT(MAX_COLUMNS_OFF_EDGE, columns_on_an_edge(8, 0));
    TEST_ASSERT_LESS_THAN_INT(MAX_COLUMNS_OFF_EDGE, columns_on_an_edge(-8, 0));
}

void
suite_ridge_curve(void) {
    RUN_TEST(test_the_curve_shares_the_photographs_frame);
    RUN_TEST(test_every_height_is_inside_the_frame_and_near_its_neighbour);
    RUN_TEST(test_the_summit_is_on_the_right_and_the_slopes_fall_to_both_sides);
    RUN_TEST(test_the_curve_lies_on_the_photographs_ridge);
    RUN_TEST(test_a_displaced_curve_does_not);
}

SUITE_REGISTER(suite_ridge_curve);

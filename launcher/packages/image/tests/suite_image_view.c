#include <stdint.h>

#include "image/image.h"
#include "image/pixel/rgb565.h"
#include "testkit/suite.h"
#include "unity.h"

static void
test_rgb565_rows_and_subimage(void) {
    enum { WIDTH = 5, HEIGHT = 4, STRIDE = 7 };

    rgb565be_t pixels[HEIGHT * STRIDE] = {0};
    const image_t image = {
        .pixels = pixels, .format = IMAGE_RGB565BE, .width = WIDTH, .height = HEIGHT, .stride = STRIDE};
    TEST_ASSERT_EQUAL_INT(sizeof *pixels * 8, image_format_bits(image.format));
    TEST_ASSERT_EQUAL_PTR(pixels, image_row(&image, 0));
    TEST_ASSERT_EQUAL_PTR(pixels + 2 * STRIDE, image_row(&image, 2));
    const recti_t rect = {1, 1, 3, 2};
    const image_t sub = image_sub(&image, rect);
    TEST_ASSERT_EQUAL_PTR(pixels + STRIDE + rect.x, image_row(&sub, 0));
    TEST_ASSERT_EQUAL_PTR(pixels + 2 * STRIDE + rect.x, image_row(&sub, 1));
    TEST_ASSERT_EQUAL_INT(rect.w, sub.width);
    TEST_ASSERT_EQUAL_INT(rect.h, sub.height);
    TEST_ASSERT_EQUAL_INT(STRIDE, sub.stride);
    TEST_ASSERT_EQUAL_INT(image.format, sub.format);
}

static void
test_mono_rows_and_subimage(void) {
    enum { WIDTH = 17, HEIGHT = 4, STRIDE = 24, BITS_PER_BYTE = 8 };

    uint8_t pixels[HEIGHT * STRIDE / BITS_PER_BYTE] = {0};
    const image_t image = {.pixels = pixels, .format = IMAGE_MONO1, .width = WIDTH, .height = HEIGHT, .stride = STRIDE};
    TEST_ASSERT_EQUAL_INT(1, image_format_bits(image.format));
    TEST_ASSERT_EQUAL_PTR(pixels, image_row(&image, 0));
    TEST_ASSERT_EQUAL_PTR(pixels + 2 * STRIDE / BITS_PER_BYTE, image_row(&image, 2));
    const recti_t rect = {BITS_PER_BYTE, 1, 9, 2};
    const image_t sub = image_sub(&image, rect);
    TEST_ASSERT_EQUAL_PTR(pixels + (STRIDE + rect.x) / BITS_PER_BYTE, image_row(&sub, 0));
    TEST_ASSERT_EQUAL_PTR(pixels + (2 * STRIDE + rect.x) / BITS_PER_BYTE, image_row(&sub, 1));
    TEST_ASSERT_EQUAL_INT(rect.w, sub.width);
    TEST_ASSERT_EQUAL_INT(rect.h, sub.height);
    TEST_ASSERT_EQUAL_INT(STRIDE, sub.stride);
    TEST_ASSERT_EQUAL_INT(image.format, sub.format);
}

void
run_image_view_suite(void) {
    RUN_TEST(test_rgb565_rows_and_subimage);
    RUN_TEST(test_mono_rows_and_subimage);
}

SUITE_REGISTER(run_image_view_suite);

#include <stdint.h>

#include "image/image.h"
#include "image/pixel/rgb565.h"
#include "testkit/suite.h"
#include "unity.h"

static void
test_tight_pitch_rounds_up_to_whole_bytes(void) {
    static const struct {
        image_format_t format;
        int width;
        uint32_t pitch;
    } cases[] = {
        {IMAGE_MONO1, 0, 0},        {IMAGE_MONO1, 1, 1},
        {IMAGE_MONO1, CHAR_BIT, 1}, {IMAGE_MONO1, CHAR_BIT + 1, 2},
        {IMAGE_INDEXED8, 3, 3},     {IMAGE_RGB565BE, 3, 6},
        {IMAGE_U16, 3, 6},          {IMAGE_U32, UINT16_MAX, UINT16_MAX * sizeof(uint32_t)},
    };

    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        TEST_ASSERT_EQUAL_UINT32(cases[i].pitch, IMAGE_PITCH(cases[i].format, cases[i].width));
    }
}

static void
test_pitch_keeps_byte_counts_when_bits_exceed_u32(void) {
    const uint32_t width = (UINT32_MAX / (sizeof(rgb565be_t) * CHAR_BIT)) + 1U;
    TEST_ASSERT_EQUAL_UINT32(width * sizeof(rgb565be_t), IMAGE_PITCH(IMAGE_RGB565BE, width));
    TEST_ASSERT_EQUAL_UINT32((UINT32_MAX / CHAR_BIT) + 1U, IMAGE_PITCH(IMAGE_MONO1, UINT32_MAX));
}

static void
test_pitch_rows_and_subimages(void) {
    enum { HEIGHT = 4, MAX_PITCH = 28 };

    uint32_t pixels[HEIGHT * MAX_PITCH / sizeof(uint32_t)] = {0};

    const struct {
        image_format_t format;
        int bits, width, pitch, x, sub_width;
    } cases[] = {
        {IMAGE_RGB565BE, 16, 5, 14, 1, 3},
        {IMAGE_MONO1, 1, 17, 3, CHAR_BIT, 9},
        {IMAGE_U16, 16, 5, 14, 1, 3},
        {IMAGE_U32, 32, 5, MAX_PITCH, 1, 3},
    };

    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        const image_t image = {pixels, cases[i].format, cases[i].width, HEIGHT, cases[i].pitch};
        const recti_t rect = {cases[i].x, 1, cases[i].sub_width, 2};
        const size_t x_bytes = cases[i].x * cases[i].bits / CHAR_BIT;
        TEST_ASSERT_EQUAL_INT(cases[i].bits, image_format_bits(image.format));
        TEST_ASSERT_EQUAL_PTR(pixels, image_row(&image, 0));
        TEST_ASSERT_EQUAL_PTR((char*)pixels + 2 * cases[i].pitch, image_row(&image, 2));
        const image_t sub = image_sub(&image, rect);
        TEST_ASSERT_EQUAL_PTR((char*)pixels + cases[i].pitch + x_bytes, image_row(&sub, 0));
        TEST_ASSERT_EQUAL_PTR((char*)pixels + 2 * cases[i].pitch + x_bytes, image_row(&sub, 1));
        TEST_ASSERT_EQUAL_INT(rect.w, sub.width);
        TEST_ASSERT_EQUAL_INT(rect.h, sub.height);
        TEST_ASSERT_EQUAL_UINT32(image.pitch, sub.pitch);
        TEST_ASSERT_EQUAL_INT(image.format, sub.format);
    }
}

/* Only the address is computed, never read, so the view needs no object behind it; a
 * real one would make the compiler flag the far row as out of its bounds. */
#define FAR_ROW_BASE ((uintptr_t)0x1000U)

static void
test_large_pitch_row_address(void) {
    const int row = 2;
    const uint32_t pitch = UINT32_MAX;
    const size_t offset = (size_t)pitch * row;
    const image_t image = {
        .pixels = (void*)FAR_ROW_BASE, .format = IMAGE_RGB565BE, .width = 1, .height = row + 1, .pitch = pitch};
    TEST_ASSERT_EQUAL_size_t(offset, (uintptr_t)image_row(&image, row) - FAR_ROW_BASE);
}

void
run_image_view_suite(void) {
    RUN_TEST(test_tight_pitch_rounds_up_to_whole_bytes);
    RUN_TEST(test_pitch_keeps_byte_counts_when_bits_exceed_u32);
    RUN_TEST(test_pitch_rows_and_subimages);
    RUN_TEST(test_large_pitch_row_address);
}

SUITE_REGISTER(run_image_view_suite);

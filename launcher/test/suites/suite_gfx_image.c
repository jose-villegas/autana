/*
 * Portable suite: gfx/image/gfx_image, the IMAG pack entry. Every check the
 * reader makes, against entries built here; on both, the boot picture's
 * shipped pack opens as one panel.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "suites.h"
#include "unity.h"

#include "asset/asset_store.h"
#include "gfx/gfx.h"
#include "gfx/image/gfx_image.h"
#include "test_alloc.h"
#include "test_pack.h"

/* A 3 x 2 picture with a stride of 4, rows from byte 16: its last pixel ends
 * the entry. */
enum {
    HEADER = 16,
    AT_FORMAT = 2,
    AT_WIDTH = 4,
    AT_HEIGHT = 6,
    AT_STRIDE = 8,
    AT_PIXELS = 12,
    WIDTH = 3,
    HEIGHT = 2,
    STRIDE = 4,
    ENTRY_BYTES = HEADER + (int)sizeof(gfx_color_t) * (STRIDE + WIDTH),
    BUFFER_BYTES = 64,
};

typedef struct {
    uint8_t* entry;
    void* raw;
} fixture_t;

static fixture_t
fixture(void) {
    fixture_t f;
    f.entry = test_alloc_aligned(BUFFER_BYTES, 16, &f.raw);
    TEST_ASSERT_NOT_NULL(f.entry);
    memset(f.entry, 0, BUFFER_BYTES);
    test_pack_put16(f.entry, GFX_IMAGE_VERSION);
    test_pack_put16(f.entry + AT_FORMAT, GFX_IMAGE_RGB565);
    test_pack_put16(f.entry + AT_WIDTH, WIDTH);
    test_pack_put16(f.entry + AT_HEIGHT, HEIGHT);
    test_pack_put32(f.entry + AT_STRIDE, STRIDE);
    test_pack_put32(f.entry + AT_PIXELS, HEADER);
    for (int i = 0; i < STRIDE + WIDTH; i++) {
        test_pack_put16(f.entry + HEADER + (i * (int)sizeof(gfx_color_t)), 0x100 + i);
    }
    return f;
}

static asset_status_t
open_fixture(const fixture_t* f, uint32_t size, gfx_image_t* out) {
    return gfx_image_open((asset_view_t){f->entry, size}, out);
}

/* The status opening the fixture gives with the u16 at `at` set to `value`. */
static asset_status_t
with_u16(int at, int value) {
    fixture_t f = fixture();
    test_pack_put16(f.entry + at, value);
    gfx_image_t image;
    const asset_status_t status = open_fixture(&f, ENTRY_BYTES, &image);
    const bool cleared = image.pixels == NULL && image.width == 0;
    test_free_aligned(f.raw);
    TEST_ASSERT_TRUE_MESSAGE(status == ASSET_OK || cleared, "a refused entry left an image behind");
    return status;
}

static asset_status_t
with_u32(int at, uint32_t value) {
    fixture_t f = fixture();
    test_pack_put32(f.entry + at, value);
    gfx_image_t image;
    const asset_status_t status = open_fixture(&f, ENTRY_BYTES, &image);
    test_free_aligned(f.raw);
    return status;
}

static void
test_an_entry_opens_and_its_pixels_point_into_it(void) {
    fixture_t f = fixture();
    gfx_image_t image;
    const asset_status_t status = open_fixture(&f, ENTRY_BYTES, &image);
    const bool in_place = (const uint8_t*)image.pixels == f.entry + HEADER;
    const gfx_color_t second_row = image.pixels[image.stride];
    test_free_aligned(f.raw);
    TEST_ASSERT_EQUAL_INT(ASSET_OK, status);
    TEST_ASSERT_TRUE_MESSAGE(in_place, "the pixels were not the entry's own bytes");
    TEST_ASSERT_EQUAL_UINT16(WIDTH, image.width);
    TEST_ASSERT_EQUAL_UINT16(HEIGHT, image.height);
    TEST_ASSERT_EQUAL_UINT32(STRIDE, image.stride);
    TEST_ASSERT_EQUAL_HEX16(0x100 + STRIDE, second_row);
}

static void
test_a_last_row_one_byte_past_the_entry_is_out_of_bounds(void) {
    fixture_t f = fixture();
    gfx_image_t image;
    const asset_status_t status = open_fixture(&f, ENTRY_BYTES - 1, &image);
    test_free_aligned(f.raw);
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_BOUNDS, status);
}

static void
test_a_truncated_header_is_out_of_bounds(void) {
    fixture_t f = fixture();
    gfx_image_t image;
    const asset_status_t status = open_fixture(&f, HEADER - 1, &image);
    test_free_aligned(f.raw);
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_BOUNDS, status);
}

static void
test_an_unknown_version_is_refused(void) {
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_VERSION, with_u16(0, GFX_IMAGE_VERSION + 1));
}

static void
test_an_unread_format_or_an_empty_image_is_a_format_error(void) {
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_FORMAT, with_u16(AT_FORMAT, 0));
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_FORMAT, with_u16(AT_FORMAT, GFX_IMAGE_MONO1 + 1));
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_FORMAT, with_u16(AT_WIDTH, 0));
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_FORMAT, with_u16(AT_HEIGHT, 0));
}

static void
test_a_stride_short_of_a_row_or_rows_over_the_header_are_a_format_error(void) {
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_FORMAT, with_u32(AT_STRIDE, WIDTH - 1));
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_FORMAT, with_u32(AT_PIXELS, HEADER - 4));
}

static void
test_misaligned_rows_are_out_of_bounds(void) {
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_BOUNDS, with_u32(AT_PIXELS, HEADER + 2));
}

static void
test_a_stride_that_wraps_round_is_out_of_bounds(void) {
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_BOUNDS, with_u32(AT_STRIDE, UINT32_MAX));
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_BOUNDS, with_u32(AT_PIXELS, UINT32_MAX - 3U));
}

static void
test_a_missing_picture_and_an_entry_of_another_type_are_told_apart(void) {
    enum { PACK_BYTES = 256 };

    void* raw = NULL;
    uint8_t* bytes = test_alloc_aligned(PACK_BYTES, ASSET_PACK_BASE_ALIGN, &raw);
    TEST_ASSERT_NOT_NULL(bytes);
    test_pack_t writer = test_pack_begin(bytes, PACK_BYTES, 2);
    const fixture_t f = fixture();
    memcpy(test_pack_add(&writer, "picture", GFX_IMAGE_ASSET, ENTRY_BYTES), f.entry, ENTRY_BYTES);
    test_free_aligned(f.raw);
    (void)test_pack_add(&writer, "other", ASSET_TYPE('O', 'T', 'H', 'R'), 4);
    const uint32_t size = test_pack_finish(&writer);
    asset_pack_t pack;
    const asset_status_t opened = asset_pack_open(&pack, bytes, size);
    gfx_image_t found, missing, other;
    const asset_status_t found_status = gfx_image_from_pack(&pack, "picture", &found);
    const asset_status_t missing_status = gfx_image_from_pack(&pack, "nope", &missing);
    const asset_status_t other_status = gfx_image_from_pack(&pack, "other", &other);
    test_free_aligned(raw);
    TEST_ASSERT_EQUAL_INT(ASSET_OK, opened);
    TEST_ASSERT_EQUAL_INT(ASSET_OK, found_status);
    TEST_ASSERT_EQUAL_UINT16(WIDTH, found.width);
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_NOT_FOUND, missing_status);
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_TYPE, other_status);
    TEST_ASSERT_NULL(other.pixels);
}

/* A 9 x 2 one-bit image, rows of 16 bits from the byte after the header:
 * one bit a pixel needs no alignment, and its last bit ends the entry. */
enum {
    MONO_WIDTH = 9,
    MONO_STRIDE = 16,
    MONO_BITS = HEADER + 1,
    MONO_BYTES = MONO_BITS + (MONO_STRIDE + MONO_WIDTH + 7) / 8,
};

/* Opens the one-bit fixture; `in_place` says whether its bits point at the
 * entry's own bytes, judged before the buffer is freed. */
static asset_status_t
open_mono(uint32_t stride, uint32_t size, gfx_image_t* out, bool* in_place) {
    fixture_t f = fixture();
    test_pack_put16(f.entry + AT_FORMAT, GFX_IMAGE_MONO1);
    test_pack_put16(f.entry + AT_WIDTH, MONO_WIDTH);
    test_pack_put32(f.entry + AT_STRIDE, stride);
    test_pack_put32(f.entry + AT_PIXELS, MONO_BITS);
    const asset_status_t status = open_fixture(&f, size, out);
    *in_place = out->bits == f.entry + MONO_BITS;
    test_free_aligned(f.raw);
    return status;
}

static void
test_a_one_bit_image_opens_unaligned_with_its_bits_in_place(void) {
    gfx_image_t image;
    bool in_place = false;
    const asset_status_t status = open_mono(MONO_STRIDE, MONO_BYTES, &image, &in_place);
    TEST_ASSERT_EQUAL_INT(ASSET_OK, status);
    TEST_ASSERT_EQUAL_INT(GFX_IMAGE_MONO1, image.format);
    TEST_ASSERT_TRUE_MESSAGE(in_place, "the bits were not the entry's own bytes");
    TEST_ASSERT_EQUAL_UINT16(MONO_WIDTH, image.width);
    TEST_ASSERT_EQUAL_UINT32(MONO_STRIDE, image.stride);
}

static void
test_a_one_bit_stride_of_part_of_a_byte_is_a_format_error(void) {
    gfx_image_t image;
    bool in_place = false;
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_FORMAT, open_mono(MONO_STRIDE - 4, MONO_BYTES, &image, &in_place));
}

static void
test_one_bit_rows_one_byte_past_the_entry_are_out_of_bounds(void) {
    gfx_image_t image;
    bool in_place = false;
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_BOUNDS, open_mono(MONO_STRIDE, MONO_BYTES - 1, &image, &in_place));
}

static void
test_the_boot_picture_s_pack_opens_as_one_panel(void) {
    const asset_pack_t* pack = asset_store_pack("boot");
    TEST_ASSERT_NOT_NULL_MESSAGE(pack, "no boot pack: see the log above");
    gfx_image_t image;
    const asset_status_t status = gfx_image_from_pack(pack, "boot", &image);
    asset_store_release("boot");
    TEST_ASSERT_EQUAL_INT(ASSET_OK, status);
    TEST_ASSERT_EQUAL_UINT16(GFX_WIDTH, image.width);
    TEST_ASSERT_EQUAL_UINT16(GFX_HEIGHT, image.height);
    TEST_ASSERT_EQUAL_UINT32(GFX_WIDTH, image.stride);
}

void
suite_gfx_image(void) {
    RUN_TEST(test_an_entry_opens_and_its_pixels_point_into_it);
    RUN_TEST(test_a_last_row_one_byte_past_the_entry_is_out_of_bounds);
    RUN_TEST(test_a_truncated_header_is_out_of_bounds);
    RUN_TEST(test_an_unknown_version_is_refused);
    RUN_TEST(test_an_unread_format_or_an_empty_image_is_a_format_error);
    RUN_TEST(test_a_stride_short_of_a_row_or_rows_over_the_header_are_a_format_error);
    RUN_TEST(test_misaligned_rows_are_out_of_bounds);
    RUN_TEST(test_a_stride_that_wraps_round_is_out_of_bounds);
    RUN_TEST(test_a_missing_picture_and_an_entry_of_another_type_are_told_apart);
    RUN_TEST(test_a_one_bit_image_opens_unaligned_with_its_bits_in_place);
    RUN_TEST(test_a_one_bit_stride_of_part_of_a_byte_is_a_format_error);
    RUN_TEST(test_one_bit_rows_one_byte_past_the_entry_are_out_of_bounds);
    RUN_TEST(test_the_boot_picture_s_pack_opens_as_one_panel);
}

SUITE_REGISTER(suite_gfx_image);

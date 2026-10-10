/*
 * Portable suite: gfx/image/gfx_image, the IMAG pack entry. Every check the
 * reader makes, against entries built here; on both, the boot picture in its
 * shipped pack opens as one panel.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "suites.h"
#include "unity.h"

#include "asset/asset_store.h"
#include "boot/boot_anim.h"
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
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_FORMAT, with_u16(AT_FORMAT, GFX_IMAGE_RGB565 + 1));
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
test_the_boot_picture_s_pack_opens_as_one_panel(void) {
    const asset_pack_t* pack = asset_store_pack(BOOT_PHOTO);
    TEST_ASSERT_NOT_NULL_MESSAGE(pack, "no boot pack: see the log above");
    gfx_image_t image = {0};
    asset_view_t entry;
    asset_status_t status = asset_pack_find(pack, BOOT_PHOTO, GFX_IMAGE_ASSET, &entry);
    if (status == ASSET_OK) {
        status = gfx_image_open(entry, &image);
    }
    asset_store_release(BOOT_PHOTO);
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
    RUN_TEST(test_the_boot_picture_s_pack_opens_as_one_panel);
}

SUITE_REGISTER(suite_gfx_image);
SUITE_READS(suite_gfx_image, BOOT_PHOTO);

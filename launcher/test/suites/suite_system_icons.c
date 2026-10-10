/*
 * Portable suite: the system icons, read from the engine pack.
 *
 * The check mark's own artwork is pinned independently in suite_icons.c
 * (hand-transcribed expected rows, not read back from the pack); this file
 * checks facts a scan can pin down across the whole set instead: run
 * counts, stride, one SVG import traced by hand against its source.
 *
 * Everything here works over EVERY baked icon by index, never assuming a
 * 16-wide/2-byte-stride shape: the atlas mixes the 16x16 PNG-sourced check
 * mark with 24x24 SVG-sourced imports, and gfx_image_t's own w/h/stride fields
 * are what make that mixing safe.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "icon_walk.h"
#include "suites.h"
#include "unity.h"

#include "bbox_extend.h"
#include "gfx/draw/icon.h"
#include "ui/ui_icons.h"

/* System icon `id`, which the engine pack the suites run with must hold. */
static const gfx_image_t*
system_icon(ui_icon_id_t id) {
    ui_icons_init();
    const gfx_image_t* icon = ui_icon(id);
    TEST_ASSERT_NOT_NULL_MESSAGE(icon, "no system icon: the suites run with the packs build_pack.py writes");
    return icon;
}

static void
test_every_icon_is_one_bit_rows_of_whole_bytes(void) {
    for (int id = 0; id < UI_ICON_COUNT; id++) {
        const gfx_image_t* icon = system_icon((ui_icon_id_t)id);
        TEST_ASSERT_EQUAL_INT_MESSAGE(GFX_IMAGE_MONO1, icon->format, "a system icon is not one bit a pixel");
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, icon->stride % GFX_IMAGE_MONO1_STRIDE_STEP, "a row is not whole bytes");
    }
}

/* main/engine/icons/system/chevron-left.svg's 7 rectangles, traced BY HAND
 * from its path data rather than by calling icons_asset.py's SVG reader: a green
 * result proves the baked bytes against the source file, not the parser's
 * own idea of what it read. */
static const char* const chevron_left_expected_rows[24] = {
    "........................", /* row 0 */
    "........................", "........................", "........................",
    "........................", "..............XX........", /* row 5 */
    "..............XX........", "............XX..........", "............XX..........",
    "..........XX............", "..........XX............", /* row 10 */
    "........XX..............", "........XX..............", "..........XX............",
    "..........XX............", "............XX..........", /* row 15 */
    "............XX..........", "..............XX........", "..............XX........",
    "........................", "........................", /* row 20 */
    "........................", "........................", "........................",
};

static void
test_chevron_left_matches_source_svg_rectangles(void) {
    const gfx_image_t* icon = system_icon(UI_ICON_CHEVRON_LEFT);
    TEST_ASSERT_EQUAL_INT(24, icon->width);
    TEST_ASSERT_EQUAL_INT(24, icon->height);

    for (int y = 0; y < 24; y++) {
        const char* row = chevron_left_expected_rows[y];
        TEST_ASSERT_EQUAL_INT_MESSAGE(24, (int)strlen(row), "a hand-transcribed expected row is not 24 characters");
        for (int x = 0; x < 24; x++) {
            const bool want = row[x] == 'X';
            const bool got = icon_test_bit(icon, x, y);
            TEST_ASSERT_EQUAL_INT_MESSAGE(want, got,
                                          "baked chevron_left diverges from chevron-left.svg's own "
                                          "rectangles - see the message above for row/col");
        }
    }
}

/* The icon's own drawn-ink scale factor: the largest integer that still
 * fits (iw,ih) inside (box_w,box_h), floored at 1. */
static int
reference_icon_scale(int iw, int ih, int box_w, int box_h) {
    int scale = box_w / iw;
    const int scale_h = box_h / ih;
    if (scale_h < scale) {
        scale = scale_h;
    }
    if (scale < 1) {
        scale = 1;
    }
    return scale;
}

/* Bounding box of the icon's own drawn ink: min/max x and y over every
 * set bit. */
static void
reference_icon_bbox(const gfx_image_t* icon, int iw, int ih, int* min_x, int* max_x, int* min_y, int* max_y) {
    *min_x = iw;
    *max_x = -1;
    *min_y = ih;
    *max_y = -1;
    for (int y = 0; y < ih; y++) {
        for (int x = 0; x < iw; x++) {
            if (icon_test_bit(icon, x, y)) {
                bbox_extend_inclusive(x, y, min_x, max_x, min_y, max_y);
            }
        }
    }
}

/* Independent geometry from icon_test_bit() lookups must agree
 * with icon_walk_blocks(), without depending on the streaming walker. */
static int
reference_blocks(const gfx_image_t* icon, int box_w, int box_h, icon_rect_t* out, int max) {
    const int iw = icon->width, ih = icon->height;
    const int scale = reference_icon_scale(iw, ih, box_w, box_h);

    int min_x, max_x, min_y, max_y;
    reference_icon_bbox(icon, iw, ih, &min_x, &max_x, &min_y, &max_y);
    TEST_ASSERT_TRUE_MESSAGE(max_x >= 0, "reference extraction found no ink in a baked icon");

    const int cw = max_x - min_x + 1;
    const int ch = max_y - min_y + 1;
    const int ox = (box_w - cw * scale) / 2 - min_x * scale;
    const int oy = (box_h - ch * scale) / 2 - min_y * scale;

    int n = 0;
    for (int y = 0; y < ih; y++) {
        int x = 0;
        while (x < iw) {
            if (!icon_test_bit(icon, x, y)) {
                x++;
                continue;
            }
            const int start = x;
            while (x < iw && icon_test_bit(icon, x, y)) {
                x++;
            }
            TEST_ASSERT_TRUE_MESSAGE(n < max, "reference extraction overflowed the test's own buffer");
            out[n].x = ox + start * scale;
            out[n].y = oy + y * scale;
            out[n].w = (x - start) * scale;
            out[n].h = scale;
            n++;
        }
    }
    return n;
}

/* home's own 50 is the largest baked run count in the atlas, so this bounds
 * every buffer below: malloc'd, not on-stack, per check_stack_usage.py's
 * gate (see its own header on the two device panics that gate exists for). */
#define TEST_MAX_BLOCKS 64

/* Proves the reshape from an array-collecting extraction to a streaming
 * callback changed no geometry: every baked icon, at its native size, at
 * 2x, and at a deliberately non-square box (exercises the scale_w != scale_h
 * clamp), must emit the exact same rects as reference_blocks() above. */
static void
test_streaming_walker_matches_reference_extraction(void) {
    icon_rect_t* cc_buf = malloc(sizeof(icon_rect_t) * TEST_MAX_BLOCKS);
    icon_rect_t* ref = malloc(sizeof(icon_rect_t) * TEST_MAX_BLOCKS);
    TEST_ASSERT_NOT_NULL(cc_buf);
    TEST_ASSERT_NOT_NULL(ref);

    for (int id = 0; id < UI_ICON_COUNT; id++) {
        const gfx_image_t* icon = system_icon((ui_icon_id_t)id);
        const int box_sizes[][2] = {
            {icon->width, icon->height},
            {icon->width * 2, icon->height * 2},
            {icon->width * 3 + 5, icon->height * 2},
        };
        for (size_t s = 0; s < sizeof(box_sizes) / sizeof(box_sizes[0]); s++) {
            const int box_w = box_sizes[s][0], box_h = box_sizes[s][1];

            icon_test_collect_t cc = {.blocks = cc_buf, .count = 0, .cap = TEST_MAX_BLOCKS};
            icon_walk_blocks(icon, box_w, box_h, icon_test_collect, &cc);

            const int ref_n = reference_blocks(icon, box_w, box_h, ref, TEST_MAX_BLOCKS);

            TEST_ASSERT_EQUAL_INT_MESSAGE(ref_n, cc.count,
                                          "icon_walk_blocks emitted a different run count than the "
                                          "reference extraction for some icon/box size");
            for (int i = 0; i < ref_n; i++) {
                TEST_ASSERT_EQUAL_INT_MESSAGE(ref[i].x, cc_buf[i].x, "run x mismatch");
                TEST_ASSERT_EQUAL_INT_MESSAGE(ref[i].y, cc_buf[i].y, "run y mismatch");
                TEST_ASSERT_EQUAL_INT_MESSAGE(ref[i].w, cc_buf[i].w, "run w mismatch");
                TEST_ASSERT_EQUAL_INT_MESSAGE(ref[i].h, cc_buf[i].h, "run h mismatch");
            }
        }
    }

    free(cc_buf);
    free(ref);
}

/* Every pixelarticons import is already centred in its own 24x24 grid, so
 * content-bbox and full-grid centring coincide for all of them; only
 * icon_check's hand-drawn ink is off-centre (1px top margin, 2px bottom),
 * which is what actually catches a caller centring on w x h instead. At a
 * 32x32 box its first run (row 1, col 13) lands at (26, 3), not (26, 2),
 * hand-derived from the centring formula. */
static void
test_content_bbox_centring_uses_a_specific_expected_origin(void) {
    const gfx_image_t* icon = system_icon(UI_ICON_CHECK);

    icon_rect_t* cc_buf = malloc(sizeof(icon_rect_t) * TEST_MAX_BLOCKS);
    TEST_ASSERT_NOT_NULL(cc_buf);
    icon_test_collect_t cc = {.blocks = cc_buf, .count = 0, .cap = TEST_MAX_BLOCKS};
    icon_walk_blocks(icon, 32, 32, icon_test_collect, &cc);

    TEST_ASSERT_TRUE_MESSAGE(cc.count > 0, "check emitted no runs");
    TEST_ASSERT_EQUAL_INT_MESSAGE(26, cc_buf[0].x, "first run x");
    TEST_ASSERT_EQUAL_INT_MESSAGE(3, cc_buf[0].y, "first run y");
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, cc_buf[0].w, "first run w");
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, cc_buf[0].h, "first run h");
    free(cc_buf);
}

/* home, the set's most detailed icon, streams all 50 of its runs: no
 * fixed buffer bounds an icon's detail. */
static void
test_home_bakes_and_walks_at_fifty_runs(void) {
    const gfx_image_t* icon = system_icon(UI_ICON_HOME);
    TEST_ASSERT_EQUAL_INT_MESSAGE(50, icon_test_runs(icon), "home was expected to bake at 50 runs");

    icon_rect_t* cc_buf = malloc(sizeof(icon_rect_t) * TEST_MAX_BLOCKS);
    TEST_ASSERT_NOT_NULL(cc_buf);
    icon_test_collect_t cc = {.blocks = cc_buf, .count = 0, .cap = TEST_MAX_BLOCKS};
    icon_walk_blocks(icon, icon->width, icon->height, icon_test_collect, &cc);
    TEST_ASSERT_EQUAL_INT_MESSAGE(50, cc.count, "icon_walk_blocks did not emit 50 runs for home at its native size");
    free(cc_buf);
}

void
run_system_icons_suite(void) {
    RUN_TEST(test_every_icon_is_one_bit_rows_of_whole_bytes);
    RUN_TEST(test_chevron_left_matches_source_svg_rectangles);
    RUN_TEST(test_streaming_walker_matches_reference_extraction);
    RUN_TEST(test_content_bbox_centring_uses_a_specific_expected_origin);
    RUN_TEST(test_home_bakes_and_walks_at_fifty_runs);
}

SUITE_REGISTER(run_system_icons_suite);

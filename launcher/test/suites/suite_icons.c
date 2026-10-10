/*
 * Portable suite: the system check icon, the artwork MU_ICON_CHECK
 * renders (see ui.c's draw_command()), read from the engine pack.
 *
 * check_expected_rows below is transcribed BY HAND from the bitmap the
 * check was first drawn as, not read back from the pack, so a match proves
 * icons_asset.py reproduced known-good pixels rather than merely
 * round-tripping its own packer; suite_system_icons.c checks facts a scan
 * can pin down instead.
 *
 * The two sizes exercised throughout are the module's two real callers:
 *   - 18px, a per-tile palette badge's icon size (scale 1; see
 *     icon_walk_blocks' own comment on why a box smaller than the bitmap
 *     still gets scale 1);
 *   - 64px, the icon rect mu_checkbox() draws at (scale 4). mu_checkbox() in
 *     components/microui/src/microui.c builds its box as
 *     mu_rect(r.x, r.y, r.h, r.h), and r.h is UI_ROW_HEIGHT (64; see ui.h),
 *     the row height a settings-style toggle screen lays its two
 *     mu_checkbox() rows out at.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "icon_walk.h"
#include "suites.h"
#include "unity.h"

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

/* check's own run count: enough at any box size, since a run's count does
 * not change with scale. */
#define CHECK_TEST_MAX_BLOCKS 16

static int
blocks_at(int w, int h, icon_rect_t* out) {
    const gfx_image_t* icon = system_icon(UI_ICON_CHECK);
    icon_test_collect_t cc = {.blocks = out, .count = 0, .cap = CHECK_TEST_MAX_BLOCKS};
    icon_walk_blocks(icon, w, h, icon_test_collect, &cc);
    return cc.count;
}

static void
assert_all_inside_box(int w, int h) {
    icon_rect_t blocks[CHECK_TEST_MAX_BLOCKS];
    const int n = blocks_at(w, h, blocks);

    TEST_ASSERT_TRUE_MESSAGE(n > 0, "produced no blocks at all");
    for (int i = 0; i < n; i++) {
        TEST_ASSERT_TRUE_MESSAGE(blocks[i].x >= 0 && blocks[i].y >= 0,
                                 "a block's origin fell outside the (0,0,w,h) box");
        TEST_ASSERT_TRUE_MESSAGE(blocks[i].x + blocks[i].w <= w && blocks[i].y + blocks[i].h <= h,
                                 "a block's far edge fell outside the (0,0,w,h) box");
    }
}

/* Sweeps every size from the module's floor (16px) up past both real call
 * sites, so containment is proven as a property rather than only at the two
 * sizes anything happens to call with today. */
static void
test_fits_inside_its_box_across_the_supported_range(void) {
    for (int side = 16; side <= 80; side++) {
        assert_all_inside_box(side, side);
    }
}

/* The bounding box of every returned block, at whatever scale, must be
 * centred within (0, 0, w, h) to within a pixel on both axes: the whole
 * point of icon_walk_blocks() scanning the bitmap's own content box rather
 * than centring the full (mostly empty) 16x16 bitmap. */
static void
assert_centred_within_a_pixel(int w, int h) {
    icon_rect_t blocks[CHECK_TEST_MAX_BLOCKS];
    const int n = blocks_at(w, h, blocks);
    TEST_ASSERT_TRUE(n > 0);

    int min_x = blocks[0].x, max_x = blocks[0].x + blocks[0].w;
    int min_y = blocks[0].y, max_y = blocks[0].y + blocks[0].h;
    for (int i = 1; i < n; i++) {
        if (blocks[i].x < min_x) {
            min_x = blocks[i].x;
        }
        if (blocks[i].y < min_y) {
            min_y = blocks[i].y;
        }
        const int right = blocks[i].x + blocks[i].w;
        const int bottom = blocks[i].y + blocks[i].h;
        if (right > max_x) {
            max_x = right;
        }
        if (bottom > max_y) {
            max_y = bottom;
        }
    }

    const int left_margin = min_x;
    const int right_margin = w - max_x;
    const int top_margin = min_y;
    const int bottom_margin = h - max_y;

    TEST_ASSERT_TRUE_MESSAGE(abs(left_margin - right_margin) <= 1,
                             "left/right margins around the mark differ by more than a pixel");
    TEST_ASSERT_TRUE_MESSAGE(abs(top_margin - bottom_margin) <= 1,
                             "top/bottom margins around the mark differ by more than a pixel");
}

static void
test_centred_at_18px(void) {
    assert_centred_within_a_pixel(18, 18);
}

static void
test_centred_at_64px(void) {
    assert_centred_within_a_pixel(64, 64);
}

static void
test_centred_at_32px(void) {
    /* A size neither real caller uses (scale 2), so centring is checked at
     * more than just the two sizes anything happens to draw at today. */
    assert_centred_within_a_pixel(32, 32);
}

/* A hand copy, not the pack's bytes; see this file's own top comment. A
 * short limb descends left-to-right to a vertex, then a long limb
 * rises past it about a third again as far. */
static const char* const check_expected_rows[16] = {
    "................",                                                                                 /* row 0 */
    ".............X..", "............XX..", "...........XX...", "...........XX...", "..........XX....", /* row 5 */
    ".........XXX....", "..XX.....XX.....", "...XX...XX......", "...XXX.XXX......", "....XXXXX.......", /* row 10 */
    ".....XXXX.......", ".....XXX........", "......XX........", "................", "................", /* row 15 */
};

static void
test_check_matches_the_artwork_it_replaced(void) {
    const gfx_image_t* icon = system_icon(UI_ICON_CHECK);
    TEST_ASSERT_EQUAL_INT(16, icon->width);
    TEST_ASSERT_EQUAL_INT(16, icon->height);
    for (int y = 0; y < 16; y++) {
        const char* row = check_expected_rows[y];
        for (int x = 0; x < 16; x++) {
            const bool want = row[x] == 'X';
            const bool got = icon_test_bit(icon, x, y);
            TEST_ASSERT_EQUAL_INT_MESSAGE(want, got,
                                          "the baked check mark diverges from the bitmap it replaced - "
                                          "see the message above for which row/col "
                                          "TEST_ASSERT_EQUAL_INT stopped at");
        }
    }
}

static int
popcount_of(const char* const* rows) {
    int set = 0;
    for (int y = 0; y < 16; y++) {
        for (int x = 0; x < 16; x++) {
            if (rows[y][x] == 'X') {
                set++;
            }
        }
    }
    return set;
}

static void
test_bitmap_is_neither_empty_nor_full(void) {
    /* 256 native pixels total. A recognisable check mark's stroke covers a
     * clear minority of its box (well under half) but is not a handful of
     * stray pixels either. 20..100 is generous either side of this bitmap's
     * actual count while still catching "went empty" or "went solid". */
    const int set = popcount_of(check_expected_rows);
    TEST_ASSERT_TRUE_MESSAGE(set >= 20 && set <= 100,
                             "the bitmap's set-pixel count is outside a sane range for a check "
                             "mark - it may have gone empty, solid, or been redrawn into "
                             "something unrecognisable");
}

/* The stroke is 2-3 native px through the body. Exactly one run is allowed
 * to be thinner than that: the long limb's free-end taper, a single pixel;
 * every other run must meet the 2px floor, or the mark reads as scattered
 * dots rather than a stroke. */
static void
test_stroke_is_at_least_2px_native_except_the_one_tapered_tip(void) {
    int thin_runs = 0;
    for (int y = 0; y < 16; y++) {
        const char* row = check_expected_rows[y];
        int x = 0;
        while (x < 16) {
            if (row[x] != 'X') {
                x++;
                continue;
            }
            const int run_start = x;
            while (x < 16 && row[x] == 'X') {
                x++;
            }
            if (x - run_start < 2) {
                thin_runs++;
            }
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(thin_runs <= 1, "more than one run in the artwork is thinner than 2px - the stroke "
                                             "should taper at exactly one free end, not read as dots throughout");
}

void
run_icons_suite(void) {
    RUN_TEST(test_fits_inside_its_box_across_the_supported_range);
    RUN_TEST(test_centred_at_18px);
    RUN_TEST(test_centred_at_64px);
    RUN_TEST(test_centred_at_32px);
    RUN_TEST(test_check_matches_the_artwork_it_replaced);
    RUN_TEST(test_bitmap_is_neither_empty_nor_full);
    RUN_TEST(test_stroke_is_at_least_2px_native_except_the_one_tapered_tip);
}

SUITE_REGISTER(run_icons_suite);

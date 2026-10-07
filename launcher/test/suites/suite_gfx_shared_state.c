/*
 * Host suite: gfx_draw.c and gfx_present.c / gfx_mode.c share ONE instance of
 * the dirty tracker, framebuffer guard, present guard and band-force latch.
 *
 * A private copy per .c file would leave every header-level suite passing
 * while a draw never told the presenter anything. These tests go through the
 * real draw primitives and observe the effect through the present/mode side,
 * so a split copy fails here and nowhere else.
 */

#include "suites.h"
#include "unity.h"

#include "gfx_shared_state.h"

#include "gfx/draw/gfx_draw.h"
#include "gfx/gfx.h"
#include "gfx/gfx_test.h"
#include "gfx/present/gfx_band_run.h"
#include "gfx/present/gfx_mode.h"
#include "gfx/present/gfx_present.h"
#include "test_cleanup.h"

#ifndef DEVICE_BUILD

enum { DRAWN_X = 10, DRAWN_Y = 10, OTHER_X = 200, OTHER_Y = 300, SIZE = 8 };

/* A presented, clean full-framebuffer frame to start every test from. */
static void
fixture(void) {
    if (gfx_mode_current()->width == 0) {
        TEST_ASSERT_TRUE(gfx_init());
    }
    suite_set_test_cleanup(gfx_reset_for_test);
    gfx_present();
    TEST_ASSERT_FALSE(gfx_region_dirty(0, 0, GFX_WIDTH, GFX_HEIGHT));
}

static void
assert_marked_only_where_drawn(void) {
    TEST_ASSERT_TRUE(gfx_region_dirty(DRAWN_X, DRAWN_Y, SIZE, SIZE));
    TEST_ASSERT_FALSE(gfx_region_dirty(OTHER_X, OTHER_Y, SIZE, SIZE));
    gfx_present();
    TEST_ASSERT_FALSE(gfx_region_dirty(0, 0, GFX_WIDTH, GFX_HEIGHT));
}

static void
test_box_marking_draw_is_seen_by_the_presenter(void) {
    fixture();
    gfx_fill_rect(DRAWN_X, DRAWN_Y, SIZE, SIZE, 0x1234);
    assert_marked_only_where_drawn();
}

static void
test_row_marking_draw_is_seen_by_the_presenter(void) {
    fixture();
    gfx_pixel(DRAWN_X, DRAWN_Y, 0x1234);
    assert_marked_only_where_drawn();
}

static const gfx_mode_request_t indexed_request = {
    .layout = GFX_LAYOUT_INDEXED,
    .index_grid_w = 1,
    .index_grid_h = 1,
    .cell_size = 1,
};

static void
test_draw_is_refused_while_a_mode_has_freed_the_framebuffer(void) {
    fixture();
    TEST_ASSERT_EQUAL_INT(GFX_LAYOUT_INDEXED, gfx_mode_enter(&indexed_request)->layout);
    const unsigned before = gfx_fb_guard_trips;
    gfx_fill_rect(DRAWN_X, DRAWN_Y, SIZE, SIZE, 0x1234);
    TEST_ASSERT_EQUAL_UINT(before + 1, gfx_fb_guard_trips);
    gfx_pixel(DRAWN_X, DRAWN_Y, 0x1234);
    TEST_ASSERT_EQUAL_UINT(before + 2, gfx_fb_guard_trips);

    gfx_mode_exit();
    const unsigned after_exit = gfx_fb_guard_trips;
    gfx_pixel(DRAWN_X, DRAWN_Y, 0x4321);
    TEST_ASSERT_EQUAL_UINT(after_exit, gfx_fb_guard_trips);
    TEST_ASSERT_EQUAL_HEX16(0x4321, gfx_framebuffer()[DRAWN_Y * GFX_WIDTH + DRAWN_X]);
}

static void
test_band_mode_refuses_draws_too(void) {
    fixture();
    const gfx_mode_request_t request = {.layout = GFX_LAYOUT_BANDS};
    TEST_ASSERT_EQUAL_INT(GFX_LAYOUT_BANDS, gfx_mode_enter(&request)->layout);
    const unsigned before = gfx_fb_guard_trips;
    gfx_fill_rect(DRAWN_X, DRAWN_Y, SIZE, SIZE, 0x1234);
    TEST_ASSERT_EQUAL_UINT(before + 1, gfx_fb_guard_trips);
    gfx_mode_exit();
}

static void
test_draw_between_present_begin_and_wait_trips_the_present_guard(void) {
    fixture();
    gfx_present_begin();
    const unsigned before = gfx_present_guard_trips;
    gfx_fill_rect(DRAWN_X, DRAWN_Y, SIZE, SIZE, 0x1234);
    TEST_ASSERT_EQUAL_UINT(before + 1, gfx_present_guard_trips);
    gfx_pixel(DRAWN_X, DRAWN_Y, 0x1234);
    TEST_ASSERT_EQUAL_UINT(before + 2, gfx_present_guard_trips);
    gfx_present_wait();

    gfx_fill_rect(DRAWN_X, DRAWN_Y, SIZE, SIZE, 0x1234);
    TEST_ASSERT_EQUAL_UINT(before + 2, gfx_present_guard_trips);
}

enum { BAND_MAX = GFX_HEIGHT / 16 };

static int band_draws;

static void
count_draw(int row0, int row1, gfx_color_t* target) {
    (void)row0;
    (void)row1;
    (void)target;
    band_draws++;
}

static int
run_bands(void) {
    band_draws = 0;
    gfx_band_run(count_draw, NULL);
    return band_draws;
}

/* gfx_invalidate() sets the force latch gfx_mode.c's band loop reads. The
 * first frames after enter are skipped: enter forces its own full frame. */
static void
test_invalidate_alone_forces_one_full_band_frame(void) {
    fixture();
    const gfx_mode_request_t request = {.layout = GFX_LAYOUT_BANDS};
    TEST_ASSERT_EQUAL_INT(GFX_LAYOUT_BANDS, gfx_mode_enter(&request)->layout);
    const int bands = gfx_mode_current()->height / gfx_mode_current()->band_height;
    TEST_ASSERT_TRUE(bands > 0 && bands <= BAND_MAX);

    for (int settle = 0; settle < 4 && run_bands() != 0; settle++) {}
    TEST_ASSERT_EQUAL_INT(0, run_bands());

    gfx_invalidate();
    TEST_ASSERT_EQUAL_INT(bands, run_bands());
    TEST_ASSERT_EQUAL_INT(0, run_bands());
    gfx_mode_exit();
}

#endif

void
run_gfx_shared_state_suite(void) {
    gfx_shared_state_t saved;
    gfx_shared_state_save(&saved);
#ifndef DEVICE_BUILD
    RUN_TEST(test_box_marking_draw_is_seen_by_the_presenter);
    RUN_TEST(test_row_marking_draw_is_seen_by_the_presenter);
    RUN_TEST(test_draw_is_refused_while_a_mode_has_freed_the_framebuffer);
    RUN_TEST(test_band_mode_refuses_draws_too);
    RUN_TEST(test_draw_between_present_begin_and_wait_trips_the_present_guard);
    RUN_TEST(test_invalidate_alone_forces_one_full_band_frame);
#endif
    gfx_shared_state_restore(&saved);
}

SUITE_REGISTER(run_gfx_shared_state_suite);

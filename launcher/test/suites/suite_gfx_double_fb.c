/* Host checks of the live double-framebuffer lifecycle and draw guard. */
#include "suites.h"

#ifndef DEVICE_BUILD
#include "unity.h"

#include "core/memory.h"
#include "gfx/present/gfx_internal.h"
#include "gfx/present/gfx_present.h"
#include "gfx/present/gfx_present_guard.h"
#include "gfx/present/gfx_test.h"
#include "heap_arena.h"
#include "test_cleanup.h"

#define FRAME_COLOUR GFX_RGB(0x204060)

static void
fixture(void) {
    gfx_present_guard_end();
    gfx_reset_for_test();
    TEST_ASSERT_TRUE(gfx_init());
    suite_set_test_cleanup(gfx_reset_for_test);
}

static void
enter_double(void) {
    const gfx_mode_request_t request = {.layout = GFX_LAYOUT_DOUBLE_FB};
    TEST_ASSERT_EQUAL_INT(GFX_LAYOUT_DOUBLE_FB, gfx_mode_enter(&request)->layout);
}

static void
test_double_fb_swaps_draw_target_and_reads_front(void) {
    fixture();
    gfx_color_t* original = gfx_present_target().buf;
    enter_double();
    gfx_color_t* drawn = gfx_present_target().buf;
    TEST_ASSERT_NOT_EQUAL(original, drawn);
    drawn[0] = FRAME_COLOUR;
    gfx_present();
    TEST_ASSERT_EQUAL_PTR(original, gfx_present_target().buf);
    original[0] = 0;
    TEST_ASSERT_EQUAL_INT(GFX_READBACK_READY, gfx_readback_begin());
    gfx_color_t* row = memory_alloc(GFX_WIDTH * sizeof(*row), MEMORY_PSRAM);
    TEST_ASSERT_NOT_NULL(row);
    gfx_read_panel_row(0, row);
    const gfx_color_t shown = row[0];
    memory_free(row);
    gfx_readback_end();
    TEST_ASSERT_EQUAL_HEX16(FRAME_COLOUR, shown);
    gfx_present();
    TEST_ASSERT_EQUAL_PTR(drawn, gfx_present_target().buf);
}

static void
test_double_fb_draw_between_presents_is_safe(void) {
    fixture();
    enter_double();
    gfx_present_begin();
    const unsigned trips = gfx_present_guard_trips;
    TEST_ASSERT_NOT_NULL(gfx_framebuffer());
    gfx_surface_t surface;
    TEST_ASSERT_TRUE(gfx_draw_begin(&surface));
    TEST_ASSERT_FALSE(surface.marks);
    TEST_ASSERT_EQUAL_UINT(trips, gfx_present_guard_trips);
    gfx_present_wait();
    TEST_ASSERT_NOT_NULL(gfx_framebuffer());
    TEST_ASSERT_EQUAL_UINT(trips, gfx_present_guard_trips);
}

static void
test_full_fb_draw_between_begin_wait_trips_guard(void) {
    fixture();
    gfx_present_begin();
    const unsigned trips = gfx_present_guard_trips;
    (void)gfx_framebuffer();
    TEST_ASSERT_EQUAL_UINT(trips + 1, gfx_present_guard_trips);
    gfx_present_wait();
}

static void
test_double_fb_exit_keeps_front_and_releases_back(void) {
    fixture();
    const size_t free_before = memory_free_bytes(MEMORY_PSRAM);
#ifdef HOST_HEAP_ARENA
    size_t blocks_before, bytes_before;
    heap_arena_snapshot(&blocks_before, &bytes_before);
#endif
    const size_t internal_before = memory_free_bytes(MEMORY_INTERNAL);
    enter_double();
    TEST_ASSERT_EQUAL_size_t(internal_before, memory_free_bytes(MEMORY_INTERNAL));
    TEST_ASSERT_TRUE(memory_free_bytes(MEMORY_PSRAM) < free_before);
    gfx_color_t* drawn = gfx_present_target().buf;
    drawn[0] = FRAME_COLOUR;
    gfx_present();
    gfx_present_target().buf[0] = 0;
    gfx_mode_exit();
    TEST_ASSERT_EQUAL_INT(GFX_LAYOUT_FULL_FB, gfx_mode_current()->layout);
    TEST_ASSERT_EQUAL_PTR(drawn, gfx_present_target().buf);
    TEST_ASSERT_EQUAL_HEX16(FRAME_COLOUR, gfx_framebuffer()[0]);
#ifdef HOST_HEAP_ARENA
    size_t blocks_after, bytes_after;
    heap_arena_snapshot(&blocks_after, &bytes_after);
    TEST_ASSERT_EQUAL_size_t(blocks_before, blocks_after);
    TEST_ASSERT_EQUAL_size_t(bytes_before, bytes_after);
#endif
    TEST_ASSERT_TRUE(gfx_region_dirty(0, 0, GFX_WIDTH, GFX_HEIGHT));
}

static void
test_double_fb_ignores_dirty_tracking_and_half_picture(void) {
    fixture();
    enter_double();
    TEST_ASSERT_NULL(gfx_half_picture());
    dirty_frame_sent();
    gfx_mark_dirty(0, 0, GFX_WIDTH, GFX_HEIGHT);
    gfx_mark_rows_dirty(0, GFX_HEIGHT);
    gfx_mark_all_dirty();
    TEST_ASSERT_FALSE(all_dirty);
    TEST_ASSERT_EQUAL_UINT(0, cell_dirty);
    TEST_ASSERT_TRUE(gfx_region_dirty(0, 0, GFX_WIDTH, GFX_HEIGHT));
}

void
run_gfx_double_fb_suite(void) {
    RUN_TEST(test_double_fb_swaps_draw_target_and_reads_front);
    RUN_TEST(test_double_fb_draw_between_presents_is_safe);
    RUN_TEST(test_full_fb_draw_between_begin_wait_trips_guard);
    RUN_TEST(test_double_fb_exit_keeps_front_and_releases_back);
    RUN_TEST(test_double_fb_ignores_dirty_tracking_and_half_picture);
}
#else
void
run_gfx_double_fb_suite(void) {}
#endif

SUITE_REGISTER(run_gfx_double_fb_suite);

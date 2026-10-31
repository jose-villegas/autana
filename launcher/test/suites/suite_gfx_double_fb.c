/* Host checks of the live double-framebuffer lifecycle and draw guard. */
#include "suites.h"

#ifndef DEVICE_BUILD
#include "unity.h"

#include "../gfx_examples/double_fb.c"
#include "core/memory.h"
#include "draw/draw.h"
#include "gfx/present/gfx_internal.h"
#include "gfx/present/gfx_present.h"
#include "gfx/present/gfx_present_guard.h"
#include "gfx/present/gfx_test.h"
#include "heap_arena.h"
#include "image/pixel/rgb565.h"
#include "testkit/cleanup.h"

#define FRAME_COLOUR RGB565BE(0x204060)
#define CLEAR_COLOUR RGB565BE(0x102030)

static void
fixture(void) {
    doubled = false;
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
    gfx_color_t* original = gfx_target_color(gfx_present_target(), 0);
    enter_double();
    gfx_color_t* drawn = gfx_target_color(gfx_present_target(), 0);
    TEST_ASSERT_NOT_EQUAL(original, drawn);
    drawn[0] = FRAME_COLOUR;
    gfx_present();
    TEST_ASSERT_EQUAL_PTR(original, gfx_target_color(gfx_present_target(), 0));
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
    TEST_ASSERT_EQUAL_PTR(drawn, gfx_target_color(gfx_present_target(), 0));
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
    TEST_ASSERT_TRUE(surface.marks);
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
    for (int presents = 0; presents <= 2; presents++) {
#ifdef HOST_HEAP_ARENA
        size_t blocks_before, bytes_before;
        heap_arena_snapshot(&blocks_before, &bytes_before);
#endif
        gfx_color_t* front = gfx_framebuffer();
        front[0] = FRAME_COLOUR;
        gfx_present();
        const size_t internal_before = memory_free_bytes(MEMORY_INTERNAL);
        enter_double();
        TEST_ASSERT_EQUAL_size_t(internal_before, memory_free_bytes(MEMORY_INTERNAL));
        for (int i = 0; i < presents; i++) {
            front = gfx_framebuffer();
            front[0] = FRAME_COLOUR;
            gfx_present();
        }
        gfx_framebuffer()[0] = 0;
        gfx_mode_exit();
        TEST_ASSERT_EQUAL_INT(GFX_LAYOUT_FULL_FB, gfx_mode_current()->layout);
        TEST_ASSERT_EQUAL_PTR(front, gfx_framebuffer());
        TEST_ASSERT_EQUAL_HEX16(FRAME_COLOUR, gfx_framebuffer()[0]);
#ifdef HOST_HEAP_ARENA
        size_t blocks_after, bytes_after;
        heap_arena_snapshot(&blocks_after, &bytes_after);
        TEST_ASSERT_EQUAL_size_t(blocks_before, blocks_after);
        TEST_ASSERT_EQUAL_size_t(bytes_before, bytes_after);
#endif
    }
}

static void
test_double_fb_cycle_and_reset_release_buffers(void) {
    fixture();
    gfx_reset_for_test();
#ifdef HOST_HEAP_ARENA
    size_t blocks_before, bytes_before;
    heap_arena_snapshot(&blocks_before, &bytes_before);
#endif
    TEST_ASSERT_TRUE(gfx_init());
    for (int cycle = 0; cycle < 2; cycle++) {
        enter_double();
        gfx_framebuffer()[0] = FRAME_COLOUR;
        gfx_present();
        gfx_mode_exit();
        TEST_ASSERT_EQUAL_HEX16(FRAME_COLOUR, gfx_framebuffer()[0]);
    }
    enter_double();
    gfx_reset_for_test();
#ifdef HOST_HEAP_ARENA
    size_t blocks_after, bytes_after;
    heap_arena_snapshot(&blocks_after, &bytes_after);
    TEST_ASSERT_EQUAL_size_t(blocks_before, blocks_after);
    TEST_ASSERT_EQUAL_size_t(bytes_before, bytes_after);
#endif
}

static void
test_double_fb_keeps_tracker_all_dirty_until_exit(void) {
    fixture();
    gfx_present();
    TEST_ASSERT_FALSE(all_dirty);
    enter_double();
    TEST_ASSERT_NULL(gfx_half_picture());
    TEST_ASSERT_TRUE(all_dirty);
    const uint32_t cells_before = cell_dirty;
    gfx_mark_dirty(0, 0, GFX_WIDTH, GFX_HEIGHT);
    gfx_mark_drawn(0, 0, GFX_WIDTH, GFX_HEIGHT);
    gfx_mark_rows_dirty(0, GFX_HEIGHT);
    gfx_mark_all_dirty();
    TEST_ASSERT_TRUE(all_dirty);
    TEST_ASSERT_EQUAL_UINT(cells_before, cell_dirty);
    gfx_present();
    gfx_present_wait();
    TEST_ASSERT_TRUE(all_dirty);
    TEST_ASSERT_TRUE(gfx_region_dirty(0, 0, GFX_WIDTH, GFX_HEIGHT));
    gfx_mode_exit();
    gfx_present();
    TEST_ASSERT_FALSE(all_dirty);
    TEST_ASSERT_FALSE(gfx_region_dirty(0, 0, GFX_WIDTH, GFX_HEIGHT));
    gfx_mark_dirty(0, 0, 1, 1);
    TEST_ASSERT_TRUE(gfx_region_dirty(0, 0, 1, 1));
}

/* Every pixel of the buffer the next frame draws into. */
static bool
back_buffer_is(gfx_color_t colour) {
    const gfx_target_t* back = gfx_present_target();
    for (int i = 0; i < GFX_WIDTH * GFX_HEIGHT; i++) {
        if (gfx_target_color(back, 0)[i] != colour) {
            return false;
        }
    }
    return true;
}

static void
test_clear_on_send_fills_each_sent_frame(void) {
    fixture();
    enter_double();
    gfx_set_clear_on_send(true, CLEAR_COLOUR);
    draw_clear(FRAME_COLOUR);
    gfx_present();
    /* The other buffer was never sent with the clear on. */
    TEST_ASSERT_FALSE(gfx_framebuffer_holds(CLEAR_COLOUR));
    draw_clear(FRAME_COLOUR);
    gfx_present();
    TEST_ASSERT_TRUE(gfx_framebuffer_holds(CLEAR_COLOUR));
    TEST_ASSERT_FALSE(gfx_framebuffer_holds(FRAME_COLOUR));
    TEST_ASSERT_TRUE(back_buffer_is(CLEAR_COLOUR));
    draw_clear(CLEAR_COLOUR);
    TEST_ASSERT_TRUE(gfx_framebuffer_holds(CLEAR_COLOUR));
    draw_pixel(0, 0, FRAME_COLOUR);
    TEST_ASSERT_FALSE(gfx_framebuffer_holds(CLEAR_COLOUR));
    /* The shown frame is cleared away behind the panel. */
    TEST_ASSERT_EQUAL_INT(GFX_READBACK_UNAVAILABLE, gfx_readback_begin());
}

static void
test_raw_framebuffer_access_forgets_the_fill(void) {
    fixture();
    enter_double();
    gfx_set_clear_on_send(true, CLEAR_COLOUR);
    gfx_present();
    gfx_present();
    TEST_ASSERT_TRUE(gfx_framebuffer_holds(CLEAR_COLOUR));
    gfx_framebuffer()[0] = FRAME_COLOUR;
    TEST_ASSERT_FALSE(gfx_framebuffer_holds(CLEAR_COLOUR));
    draw_clear(CLEAR_COLOUR);
    TEST_ASSERT_TRUE(back_buffer_is(CLEAR_COLOUR));
}

static void
test_clear_on_send_ends_with_its_mode(void) {
    fixture();
    TEST_ASSERT_FALSE(gfx_set_clear_on_send(true, CLEAR_COLOUR)); /* full framebuffer: refused */
    gfx_present();
    gfx_present();
    TEST_ASSERT_FALSE(gfx_framebuffer_holds(CLEAR_COLOUR));
    enter_double();
    TEST_ASSERT_TRUE(gfx_set_clear_on_send(true, CLEAR_COLOUR));
    gfx_present();
    gfx_present();
    gfx_mode_exit();
    /* The frame kept at exit was cleared on send: nothing to read back. */
    TEST_ASSERT_TRUE(gfx_framebuffer_holds(CLEAR_COLOUR));
    TEST_ASSERT_TRUE(back_buffer_is(CLEAR_COLOUR));
    TEST_ASSERT_EQUAL_INT(GFX_READBACK_UNAVAILABLE, gfx_readback_begin());
    draw_pixel(0, 0, FRAME_COLOUR);
    gfx_present();
    TEST_ASSERT_EQUAL_INT(GFX_READBACK_READY, gfx_readback_begin());
    gfx_readback_end();
    enter_double();
    draw_clear(FRAME_COLOUR);
    gfx_present();
    draw_clear(FRAME_COLOUR);
    gfx_present();
    TEST_ASSERT_FALSE(gfx_framebuffer_holds(CLEAR_COLOUR));
    TEST_ASSERT_TRUE(back_buffer_is(FRAME_COLOUR));
}

static void
test_doc_double_fb_clears_and_exits(void) {
    fixture();
    effect_enter();
    TEST_ASSERT_TRUE(doubled);
    TEST_ASSERT_EQUAL_INT(GFX_LAYOUT_DOUBLE_FB, gfx_mode_current()->layout);
    draw_clear(FRAME_COLOUR);
    gfx_present();
    draw_clear(FRAME_COLOUR);
    gfx_present();
    TEST_ASSERT_TRUE(gfx_framebuffer_holds(RGB565BE(0x000000)));
    TEST_ASSERT_TRUE(back_buffer_is(RGB565BE(0x000000)));
    effect_exit();
    TEST_ASSERT_EQUAL_INT(GFX_LAYOUT_FULL_FB, gfx_mode_current()->layout);
    draw_pixel(0, 0, FRAME_COLOUR);
    gfx_present();
    TEST_ASSERT_EQUAL_INT(GFX_READBACK_READY, gfx_readback_begin());
    gfx_readback_end();
}

void
run_gfx_double_fb_suite(void) {
    RUN_TEST(test_doc_double_fb_clears_and_exits);
    RUN_TEST(test_double_fb_swaps_draw_target_and_reads_front);
    RUN_TEST(test_double_fb_draw_between_presents_is_safe);
    RUN_TEST(test_full_fb_draw_between_begin_wait_trips_guard);
    RUN_TEST(test_double_fb_exit_keeps_front_and_releases_back);
    RUN_TEST(test_double_fb_cycle_and_reset_release_buffers);
    RUN_TEST(test_double_fb_keeps_tracker_all_dirty_until_exit);
    RUN_TEST(test_clear_on_send_fills_each_sent_frame);
    RUN_TEST(test_raw_framebuffer_access_forgets_the_fill);
    RUN_TEST(test_clear_on_send_ends_with_its_mode);
}
#else
void
run_gfx_double_fb_suite(void) {}
#endif

SUITE_REGISTER(run_gfx_double_fb_suite);

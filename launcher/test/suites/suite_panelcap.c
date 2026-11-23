#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../fixtures/panelcap_fixture.h"
#include "core/memory.h"
#include "gfx/present/capture/panelcap.h"
#include "math/scalar/units.h"
#include "suites.h"
#include "testkit/cleanup.h"
#include "unity.h"

#define TEST_WINDOW_SPACING 2U

#ifndef DEVICE_BUILD
bool console_panelcap_on;
static int panel_result;
#endif

static panelcap_t* capture;
static uint8_t* storage;

static void
release(void) {
    memory_free(storage);
    storage = NULL;
    memory_free(capture);
    capture = NULL;
#ifndef DEVICE_BUILD
    console_panelcap_on = false;
    panel_result = 0;
#endif
}

static void
fixture(void) {
    suite_set_test_cleanup(release);
    capture = memory_alloc(sizeof *capture, MEMORY_APP);
    TEST_ASSERT_NOT_NULL(capture);
    storage = memory_alloc(PANELCAP_IMAGE_BYTES, MEMORY_APP);
    TEST_ASSERT_NOT_NULL(storage);
    TEST_ASSERT_TRUE(panelcap_init(capture, storage, PANELCAP_FPS_DEFAULT));
}

typedef struct {
    uint8_t* bytes;
    size_t used;
    size_t capacity;
} sink_t;

static void
collect(void* arg, const uint8_t* bytes, size_t size) {
    sink_t* sink = arg;
    TEST_ASSERT_LESS_OR_EQUAL(sink->capacity - sink->used, size);
    memcpy(sink->bytes + sink->used, bytes, size);
    sink->used += size;
}

static void
test_busy_sender_drops_whole_present(void) {
    fixture();
    const uint8_t pixel[] = {0x12, 0x34};
    TEST_ASSERT_FALSE(panelcap_begin(capture, 0));
    panelcap_window(capture, 0, 0, 1, 1, pixel);
    panelcap_commit(capture);
    TEST_ASSERT_FALSE(panelcap_begin(capture, MATH_US_PER_S));
    const uint8_t refused[] = {0xab, 0xcd};
    panelcap_window(capture, 5, 7, 6, 8, refused);
    panelcap_window(capture, 5, 7, 6, 8, refused);
    panelcap_commit(capture);
    TEST_ASSERT_EQUAL_MEMORY(pixel, capture->storage, sizeof pixel);
    TEST_ASSERT_EQUAL(1, bytes_read_u32(panelcap_peek(capture)));
    TEST_ASSERT_EQUAL(1, capture->presents);
    TEST_ASSERT_EQUAL(1, capture->dropped);
    TEST_ASSERT_EQUAL(1, capture->dropped_busy);
    panelcap_release(capture);
    TEST_ASSERT_TRUE(panelcap_begin(capture, 2 * MATH_US_PER_S));
    panelcap_commit(capture);
    TEST_ASSERT_NULL(panelcap_peek(capture));
    TEST_ASSERT_FALSE(panelcap_begin(capture, 2 * MATH_US_PER_S + 1));
    panelcap_commit(capture);
    TEST_ASSERT_NULL(panelcap_peek(capture));
    const recti_t redraw = panelcap_redraw_requested(capture);
    TEST_ASSERT_EQUAL_INT(5, redraw.x);
    TEST_ASSERT_EQUAL_INT(7, redraw.y);
    TEST_ASSERT_EQUAL_INT(1, redraw.w);
    TEST_ASSERT_EQUAL_INT(1, redraw.h);
    TEST_ASSERT_FALSE(panelcap_begin(capture, 2 * MATH_US_PER_S + 2));
    panelcap_window(capture, 0, 0, 1, 1, refused);
    panelcap_commit(capture);
    TEST_ASSERT_EQUAL_MEMORY(refused, capture->storage, sizeof refused);
    TEST_ASSERT_EQUAL(1, bytes_read_u16(panelcap_peek(capture) + PANELCAP_HEAD_AT_DROPPED));
}

/* Indexed and framebuffer layouts resend a lost box from the picture they
 * keep; only bands, which keep none, need the app to draw it again. */
static void
test_lost_box_redraws_without_the_app_where_a_picture_is_kept(void) {
    TEST_ASSERT_FALSE(panelcap_redraw_needs_app(GFX_LAYOUT_FULL_FB));
    TEST_ASSERT_FALSE(panelcap_redraw_needs_app(GFX_LAYOUT_DOUBLE_FB));
    TEST_ASSERT_FALSE(panelcap_redraw_needs_app(GFX_LAYOUT_INDEXED));
    TEST_ASSERT_TRUE(panelcap_redraw_needs_app(GFX_LAYOUT_BANDS));
}

static void
test_window_after_sender_release_joins_current_present(void) {
    fixture();
    const uint8_t pixel[] = {0x12, 0x34};
    panelcap_begin(capture, 0);
    panelcap_window(capture, 0, 0, 1, 1, pixel);
    panelcap_commit(capture);

    TEST_ASSERT_FALSE(panelcap_begin(capture, MATH_US_PER_S));
    panelcap_release(capture);
    panelcap_window(capture, 2, 0, 3, 1, pixel);
    panelcap_commit(capture);
    TEST_ASSERT_NOT_NULL(panelcap_peek(capture));
    TEST_ASSERT_EQUAL(2, capture->presents);
    TEST_ASSERT_EQUAL(0, capture->dropped);
    TEST_ASSERT_EQUAL(1, capture->windows);
    TEST_ASSERT_EQUAL(2, capture->dirty[0].x);
    TEST_ASSERT_EQUAL_MEMORY(pixel, storage + 2 * PANELCAP_PIXEL_BYTES, sizeof pixel);
}

static void
test_idle_present_does_not_consume_fps_slot(void) {
    fixture();
    const uint8_t pixel[] = {0x12, 0x34};
    panelcap_begin(capture, 0);
    panelcap_commit(capture);
    TEST_ASSERT_NULL(panelcap_peek(capture));
    TEST_ASSERT_EQUAL(0, capture->presents);
    TEST_ASSERT_EQUAL(0, capture->dropped);
    panelcap_begin(capture, 1);
    panelcap_window(capture, 0, 0, 1, 1, pixel);
    panelcap_commit(capture);
    TEST_ASSERT_NOT_NULL(panelcap_peek(capture));
    panelcap_begin(capture, 2);
    panelcap_commit(capture);
    TEST_ASSERT_EQUAL(0, capture->dropped);
}

static void
test_fps_gate_counts_dropped_presents(void) {
    fixture();
    const uint8_t pixel[] = {0x12, 0x34};
    TEST_ASSERT_FALSE(panelcap_begin(capture, 0));
    panelcap_window(capture, 0, 0, 1, 1, pixel);
    panelcap_commit(capture);
    panelcap_release(capture);
    TEST_ASSERT_FALSE(panelcap_begin(capture, 1));
    panelcap_window(capture, 0, 0, 1, 1, pixel);
    panelcap_commit(capture);
    TEST_ASSERT_NULL(panelcap_peek(capture));
    TEST_ASSERT_FALSE(panelcap_begin(capture, MATH_US_PER_S / PANELCAP_FPS_DEFAULT));
    panelcap_commit(capture);
    TEST_ASSERT_EQUAL(1, bytes_read_u16(panelcap_peek(capture) + PANELCAP_HEAD_AT_DROPPED));
    TEST_ASSERT_EQUAL(1, capture->dropped_fps);
}

static void
test_window_overflow_collapses_to_bounding_box(void) {
    fixture();
    const uint8_t pixel[] = {0x12, 0x34};
    TEST_ASSERT_FALSE(panelcap_begin(capture, 0));
    for (unsigned i = 0; i <= PANELCAP_WINDOWS_MAX; i++) {
        unsigned x = (i * TEST_WINDOW_SPACING) % PANELCAP_WIDTH;
        unsigned y = (i * TEST_WINDOW_SPACING) / PANELCAP_WIDTH;
        panelcap_window(capture, x, y, x + 1, y + 1, pixel);
    }
    TEST_ASSERT_EQUAL(1, capture->windows);
    TEST_ASSERT_EQUAL(0, capture->dirty[0].x);
    TEST_ASSERT_EQUAL(0, capture->dirty[0].y);
    TEST_ASSERT_EQUAL(PANELCAP_WINDOWS_MAX * TEST_WINDOW_SPACING + 1, capture->dirty[0].w);
    TEST_ASSERT_EQUAL(1, capture->dirty[0].h);
    panelcap_commit(capture);
    TEST_ASSERT_NOT_NULL(panelcap_peek(capture));
    TEST_ASSERT_EQUAL(0, capture->dropped);
}

typedef struct {
    sink_t sink;
    unsigned part;
    bool last;
} join_t;

static void
join(void* arg, const uint8_t* bytes, size_t size) {
    join_t* joined = arg;
    TEST_ASSERT_FALSE(joined->last);
    TEST_ASSERT_LESS_OR_EQUAL(PANELCAP_FIXTURE_PART_BYTES, size);
    TEST_ASSERT_EQUAL(1, bytes_read_u32(bytes + PANELCAP_PART_AT_SEQ));
    TEST_ASSERT_EQUAL(joined->part++, bytes_read_u16(bytes + PANELCAP_PART_AT_INDEX));
    joined->last = bytes[PANELCAP_PART_AT_LAST];
    collect(&joined->sink, bytes + PANELCAP_PART_HEAD_BYTES, size - PANELCAP_PART_HEAD_BYTES);
}

static void
join_present(join_t* joined) {
    panelcap_encoder_t encoder = {0};
    uint8_t packet[PANELCAP_FIXTURE_PART_BYTES];
    size_t count;
    while ((count = panelcap_fill(capture, &encoder, packet, sizeof packet)) != 0) {
        join(joined, packet, count);
    }
}

static void
test_shared_packbits_wire(void) {
    fixture();
    panelcap_begin(capture, 0);
    for (size_t at = 0; at < sizeof panelcap_fixture_pixels;) {
        const unsigned y = bytes_read_u16(panelcap_fixture_pixels + at);
        at += sizeof(uint16_t);
        const unsigned width = bytes_read_u16(panelcap_fixture_pixels + at);
        at += sizeof(uint16_t);
        panelcap_window(capture, 0, y, width, y + 1, panelcap_fixture_pixels + at);
        at += width * PANELCAP_PIXEL_BYTES;
    }
    panelcap_commit(capture);
    uint8_t result[sizeof panelcap_fixture_wire - 1];
    join_t joined = {.sink = {.bytes = result, .capacity = sizeof result}};
    join_present(&joined);
    TEST_ASSERT_TRUE(joined.last);
    TEST_ASSERT_GREATER_THAN(1, joined.part);
    TEST_ASSERT_EQUAL(sizeof result, joined.sink.used);
    TEST_ASSERT_EQUAL(INT8_MIN, (int8_t)panelcap_fixture_wire[PANELCAP_FIXTURE_NOOP_AT]);
    TEST_ASSERT_EQUAL_MEMORY(panelcap_fixture_wire, result, PANELCAP_FIXTURE_NOOP_AT);
    TEST_ASSERT_EQUAL_MEMORY(panelcap_fixture_wire + PANELCAP_FIXTURE_NOOP_AT + 1, result + PANELCAP_FIXTURE_NOOP_AT,
                             sizeof result - PANELCAP_FIXTURE_NOOP_AT);
}

static void
test_continuation_rejoins_encoded_present(void) {
    fixture();
    const uint8_t pixels[] = {0x12, 0x34};
    TEST_ASSERT_FALSE(panelcap_begin(capture, 0));
    panelcap_window(capture, 0, 0, 1, 1, pixels);
    panelcap_commit(capture);
    uint8_t result[PANELCAP_HEAD_BYTES + PANELCAP_WINDOW_BYTES + 1 + sizeof pixels];
    join_t joined = {.sink = {.bytes = result, .capacity = sizeof result}};
    join_present(&joined);
    TEST_ASSERT_TRUE(joined.last);
    TEST_ASSERT_EQUAL(2, joined.part);
    TEST_ASSERT_EQUAL(sizeof result, joined.sink.used);
    TEST_ASSERT_EQUAL(0, result[PANELCAP_HEAD_BYTES + PANELCAP_WINDOW_BYTES]);
    TEST_ASSERT_EQUAL_MEMORY(pixels, result + PANELCAP_HEAD_BYTES + PANELCAP_WINDOW_BYTES + 1, sizeof pixels);
}

static void
test_overlapping_windows_keep_last_pixel(void) {
    fixture();
    const uint8_t first[] = {0x12, 0x34};
    const uint8_t last[] = {0x56, 0x78};
    TEST_ASSERT_FALSE(panelcap_begin(capture, 0));
    panelcap_window(capture, 0, 0, 1, 1, first);
    panelcap_window(capture, 0, 0, 1, 1, last);
    panelcap_commit(capture);
    uint8_t result[PANELCAP_HEAD_BYTES + 2 * (PANELCAP_WINDOW_BYTES + 1 + sizeof first)];
    join_t joined = {.sink = {.bytes = result, .capacity = sizeof result}};
    join_present(&joined);
    TEST_ASSERT_EQUAL(1, bytes_read_u16(result + PANELCAP_HEAD_AT_WINDOWS));
    TEST_ASSERT_EQUAL_MEMORY(last, result + PANELCAP_HEAD_BYTES + PANELCAP_WINDOW_BYTES + 1, sizeof last);
}

static void
test_image_rows_and_invalid_window_ignored(void) {
    fixture();
    const uint8_t rows[] = {0x12, 0x34, 0x56, 0x78};
    TEST_ASSERT_FALSE(panelcap_begin(capture, 0));
    panelcap_window(capture, 1, 1, 2, 3, rows);
    TEST_ASSERT_EQUAL_MEMORY(rows, capture->storage + (PANELCAP_WIDTH + 1) * PANELCAP_PIXEL_BYTES,
                             PANELCAP_PIXEL_BYTES);
    TEST_ASSERT_EQUAL_MEMORY(rows + PANELCAP_PIXEL_BYTES,
                             capture->storage + (2 * PANELCAP_WIDTH + 1) * PANELCAP_PIXEL_BYTES, PANELCAP_PIXEL_BYTES);
    panelcap_window(capture, 0, 0, PANELCAP_WIDTH + 1, 1, rows);
    panelcap_commit(capture);
    TEST_ASSERT_NOT_NULL(panelcap_peek(capture));
    TEST_ASSERT_EQUAL(1, capture->windows);
    TEST_ASSERT_EQUAL(0, capture->dropped);
}

static void
test_wire_fields_are_little_endian(void) {
    fixture();
    const uint8_t pixel[] = {0x12, 0x34};
    const uint64_t device_us = UINT64_C(0x0102030405060708);
    const uint32_t previous_seq = UINT32_C(0x44332210);
    const uint8_t expected[] = {0x11, 0x22, 0x33, 0x44, 8, 7, 6, 5, 4, 3, 2, 1, 1, 0, 0, 0};
    capture->seq = previous_seq;
    TEST_ASSERT_FALSE(panelcap_begin(capture, device_us));
    panelcap_window(capture, 0, 0, 1, 1, pixel);
    panelcap_commit(capture);
    TEST_ASSERT_EQUAL_MEMORY(expected, panelcap_peek(capture), sizeof expected);
}

static void
test_throttled_cursor_leaves_no_ghost(void) {
    fixture();
    const uint8_t cursor[] = {0xff, 0xff};
    const uint8_t background[] = {0, 0};
    const unsigned period = MATH_US_PER_S / PANELCAP_FPS_DEFAULT;
    const unsigned presents_per_period = PANELCAP_FPS_MAX / PANELCAP_FPS_DEFAULT;
    for (unsigned x = 0; x <= presents_per_period; x++) {
        panelcap_begin(capture, x * period / presents_per_period);
        if (x != 0) {
            panelcap_window(capture, x - 1, 0, x, 1, background);
        }
        panelcap_window(capture, x, 0, x + 1, 1, cursor);
        panelcap_commit(capture);
        if (x == 0) {
            panelcap_release(capture);
        }
    }
    TEST_ASSERT_EQUAL_MEMORY(background, capture->storage, sizeof background);
    TEST_ASSERT_EQUAL(2, capture->presents);
}

static void
test_drop_saturation_and_reset(void) {
    fixture();
    const uint8_t pixel[] = {0x12, 0x34};
    TEST_ASSERT_FALSE(panelcap_init(capture, storage, 0));
    const unsigned period = MATH_US_PER_S / PANELCAP_FPS_DEFAULT;
    panelcap_begin(capture, 0);
    panelcap_window(capture, 0, 0, 1, 1, pixel);
    panelcap_commit(capture);
    panelcap_release(capture);
    for (unsigned i = 0; i <= UINT16_MAX; i++) {
        panelcap_begin(capture, 1);
        panelcap_window(capture, 0, 0, 1, 1, pixel);
        panelcap_commit(capture);
    }
    panelcap_begin(capture, period);
    panelcap_commit(capture);
    TEST_ASSERT_EQUAL(UINT16_MAX, bytes_read_u16(panelcap_peek(capture) + PANELCAP_HEAD_AT_DROPPED));
    TEST_ASSERT_EQUAL((unsigned)UINT16_MAX + 1, capture->dropped);
    panelcap_release(capture);
    panelcap_begin(capture, period + 1);
    panelcap_window(capture, 0, 0, 1, 1, pixel);
    panelcap_commit(capture);
    panelcap_begin(capture, period * 2);
    panelcap_commit(capture);
    TEST_ASSERT_EQUAL(1, bytes_read_u16(panelcap_peek(capture) + PANELCAP_HEAD_AT_DROPPED));
}

#ifndef DEVICE_BUILD
#include "gfx/present/capture/gfx_panel_capture.h"
#include "gfx/present/gfx_debug.h"
#include "gfx/present/gfx_panel.h"

int
gfx_panel_host_draw(int x0, int y0, int x1, int y1, const gfx_color_t* pixels) {
    return panel_result;
}

void
console_panelcap_begin_active(void) {
    panelcap_begin(capture, 0);
}

void
console_panelcap_commit_active(void) {
    panelcap_commit(capture);
}

void
console_panelcap_window(int x0, int y0, int x1, int y1, const gfx_color_t* pixels) {
    panelcap_window(capture, x0, y0, x1, y1, pixels);
}

static void
test_dispatch_only_captures_successful_windows_inside_one_present(void) {
    fixture();
    const gfx_color_t pixel = UINT16_MAX;
    const int64_t initial = gfx_get_bytes_sent();
    console_panelcap_on = false;
    gfx_panel_capture_begin();
    gfx_panel_send(0, 0, 1, 1, &pixel);
    gfx_panel_capture_commit();
    TEST_ASSERT_EQUAL(sizeof pixel, gfx_get_bytes_sent() - initial);
    TEST_ASSERT_EQUAL(0, bytes_read_u16(storage));
    TEST_ASSERT_NULL(panelcap_peek(capture));

    console_panelcap_on = true;
    gfx_panel_send(0, 0, 1, 1, &pixel);
    gfx_panel_capture_begin();
    gfx_panel_send(1, 0, 2, 1, &pixel);
    gfx_panel_send(2, 0, 3, 1, &pixel);
    panel_result = -1;
    gfx_panel_send(3, 0, 4, 1, &pixel);
    panel_result = 0;
    TEST_ASSERT_NULL(panelcap_peek(capture));
    gfx_panel_capture_commit();
    console_panelcap_on = false;
    TEST_ASSERT_NOT_NULL(panelcap_peek(capture));
    TEST_ASSERT_EQUAL(1, capture->presents);
    TEST_ASSERT_EQUAL(1, capture->windows);
    const gfx_color_t expected[] = {0, pixel, pixel, 0};
    TEST_ASSERT_EQUAL_MEMORY(expected, storage, sizeof expected);
}
#endif

void
suite_panelcap(void) {
#ifndef DEVICE_BUILD
    RUN_TEST(test_dispatch_only_captures_successful_windows_inside_one_present);
#endif
    RUN_TEST(test_throttled_cursor_leaves_no_ghost);
    RUN_TEST(test_shared_packbits_wire);
    RUN_TEST(test_drop_saturation_and_reset);
    RUN_TEST(test_busy_sender_drops_whole_present);
    RUN_TEST(test_lost_box_redraws_without_the_app_where_a_picture_is_kept);
    RUN_TEST(test_window_after_sender_release_joins_current_present);
    RUN_TEST(test_idle_present_does_not_consume_fps_slot);
    RUN_TEST(test_fps_gate_counts_dropped_presents);
    RUN_TEST(test_window_overflow_collapses_to_bounding_box);
    RUN_TEST(test_continuation_rejoins_encoded_present);
    RUN_TEST(test_overlapping_windows_keep_last_pixel);
    RUN_TEST(test_image_rows_and_invalid_window_ignored);
    RUN_TEST(test_wire_fields_are_little_endian);
}

SUITE_REGISTER(suite_panelcap);

#ifdef DEVICE_BUILD
#include "app/app_arena.h"
#include "app/console/console_panelcap_device.h"
#include "draw/draw.h"
#include "gfx/gfx.h"
#include "gfx/present/capture/gfx_panel_capture.h"
#include "gfx/present/gfx_debug.h"
#include "gfx/present/gfx_internal.h"
#include "gfx/present/gfx_present.h"
#include "image/pixel/rgb565.h"
#include "shell/shell_apps.h"

static char* capture_reply;
static void* app_block;
static bool capture_at_app_exit;
static size_t free_at_app_exit;
static size_t largest_at_app_exit;
static void** internal_blocks;

static void
save_capture_reply(const char* line) {
    if (capture_reply != NULL) {
        (void)snprintf(capture_reply, PANELCAP_LINE_BYTES, "%s", line);
    }
}

static void
release_capture(void) {
    if (console_panelcap_on) {
        console_panelcap_stop();
    }
    memory_free(app_block);
    app_block = NULL;
    while (internal_blocks != NULL) {
        void** block = internal_blocks;
        internal_blocks = *block;
        memory_free(block);
    }
    if (!app_heap_active()) {
        app_arena_enter();
    }
}

static void
test_start_stop_restores_app_memory(void) {
    suite_set_test_cleanup(release_capture);
    const size_t largest = memory_largest_block(MEMORY_APP);
    const size_t free = memory_free_bytes(MEMORY_APP);
    const size_t mark = app_arena_mark();
    console_panelcap_start(PANELCAP_FPS_DEFAULT, save_capture_reply);
    TEST_ASSERT_TRUE(console_panelcap_on);
    TEST_ASSERT_GREATER_OR_EQUAL_size_t(PANELCAP_CAPTURE_BYTES, free - memory_free_bytes(MEMORY_APP));
    TEST_ASSERT_LESS_THAN_size_t(largest, memory_largest_block(MEMORY_APP));
    TEST_ASSERT_EQUAL_size_t(mark, app_arena_mark());
    console_panelcap_stop();
    TEST_ASSERT_FALSE(console_panelcap_on);
    TEST_ASSERT_EQUAL_size_t(largest, memory_largest_block(MEMORY_APP));
    TEST_ASSERT_EQUAL_size_t(free, memory_free_bytes(MEMORY_APP));
}

static void
test_start_refuses_when_app_memory_is_short(void) {
    suite_set_test_cleanup(release_capture);
    const size_t largest = memory_largest_block(MEMORY_APP);
    app_block = memory_alloc(largest, MEMORY_APP);
    TEST_ASSERT_NOT_NULL(app_block);
    const size_t remaining = memory_largest_block(MEMORY_APP);
    TEST_ASSERT_LESS_THAN_size_t(PANELCAP_CAPTURE_BYTES, remaining);
    const size_t free = memory_free_bytes(MEMORY_APP);

    char actual[PANELCAP_LINE_BYTES] = {0};
    capture_reply = actual;
    console_panelcap_start(PANELCAP_FPS_DEFAULT, save_capture_reply);
    capture_reply = NULL;
    TEST_ASSERT_FALSE(console_panelcap_on);
    char expected[PANELCAP_LINE_BYTES];
    (void)snprintf(expected, sizeof expected, PANELCAP_NO_MEMORY, PANELCAP_CAPTURE_BYTES, remaining);
    TEST_ASSERT_EQUAL_STRING(expected, actual);
    TEST_ASSERT_EQUAL_size_t(free, memory_free_bytes(MEMORY_APP));
}

static void
test_launcher_stop_restores_arena(void) {
    suite_set_test_cleanup(release_capture);
    TEST_ASSERT_EQUAL_size_t(0, app_heap_blocks());
    app_arena_exit();
    const bool active = app_heap_active();
    const size_t bytes = app_arena_bytes();
    console_panelcap_start(PANELCAP_FPS_DEFAULT, save_capture_reply);
    TEST_ASSERT_TRUE(console_panelcap_on);
    TEST_ASSERT_TRUE(app_heap_active());
    console_panelcap_stop();
    TEST_ASSERT_EQUAL(active, app_heap_active());
    TEST_ASSERT_EQUAL_size_t(bytes, app_arena_bytes());
}

static void
test_launcher_sender_refusal_restores_arena(void) {
    suite_set_test_cleanup(release_capture);
    TEST_ASSERT_EQUAL_size_t(0, app_heap_blocks());
    app_arena_exit();
    const bool active = app_heap_active();
    const size_t bytes = app_arena_bytes();
    size_t largest;
    while ((largest = memory_largest_block(MEMORY_INTERNAL)) >= PANELCAP_LINE_BYTES) {
        void** block = memory_alloc(largest, MEMORY_INTERNAL);
        TEST_ASSERT_NOT_NULL(block);
        *block = internal_blocks;
        internal_blocks = block;
    }

    char actual[PANELCAP_LINE_BYTES] = {0};
    capture_reply = actual;
    console_panelcap_start(PANELCAP_FPS_DEFAULT, save_capture_reply);
    capture_reply = NULL;
    TEST_ASSERT_FALSE(console_panelcap_on);
    TEST_ASSERT_EQUAL_STRING(PANELCAP_NO_SENDER, actual);
    TEST_ASSERT_EQUAL(active, app_heap_active());
    TEST_ASSERT_EQUAL_size_t(bytes, app_arena_bytes());
}

static void
capture_app_enter(void) {}

static void
capture_app_exit(void) {
    capture_at_app_exit = console_panelcap_on;
    free_at_app_exit = memory_free_bytes(MEMORY_APP);
    largest_at_app_exit = memory_largest_block(MEMORY_APP);
    if (capture_at_app_exit) {
        console_panelcap_stop();
    }
}

static void
test_app_switch_stops_capture(void) {
    static const app_t app = {.name = "Panel capture test", .enter = capture_app_enter, .exit = capture_app_exit};
    const app_t* current = NULL;
    TEST_ASSERT_EQUAL_size_t(0, app_heap_blocks());
    app_arena_exit();
    shell_start_app(&current, &app);
    const size_t free = memory_free_bytes(MEMORY_APP);
    const size_t largest = memory_largest_block(MEMORY_APP);
    console_panelcap_start(PANELCAP_FPS_DEFAULT, save_capture_reply);
    const bool started = console_panelcap_on;
    shell_exit_app(&current);
    const bool captured_at_exit = capture_at_app_exit;
    const size_t exited_free = free_at_app_exit;
    const size_t exited_largest = largest_at_app_exit;

    console_panelcap_start(PANELCAP_FPS_DEFAULT, save_capture_reply);
    const bool started_from_launcher = console_panelcap_on;
    shell_start_app(&current, &app);
    const bool captured_after_enter = console_panelcap_on;
    const size_t entered_free = memory_free_bytes(MEMORY_APP);
    const size_t entered_largest = memory_largest_block(MEMORY_APP);
    shell_exit_app(&current);
    app_arena_enter();

    TEST_ASSERT_TRUE(started);
    TEST_ASSERT_FALSE(captured_at_exit);
    TEST_ASSERT_EQUAL_size_t(free, exited_free);
    TEST_ASSERT_EQUAL_size_t(largest, exited_largest);
    TEST_ASSERT_TRUE(started_from_launcher);
    TEST_ASSERT_FALSE(captured_after_enter);
    TEST_ASSERT_EQUAL_size_t(free, entered_free);
    TEST_ASSERT_EQUAL_size_t(largest, entered_largest);
}

void
suite_panelcap_lifecycle(void) {
    RUN_TEST(test_start_stop_restores_app_memory);
    RUN_TEST(test_start_refuses_when_app_memory_is_short);
    RUN_TEST(test_launcher_stop_restores_arena);
    RUN_TEST(test_launcher_sender_refusal_restores_arena);
    RUN_TEST(test_app_switch_stops_capture);
}

SUITE_REGISTER_ON_REQUEST(suite_panelcap_lifecycle);

#define PANELCAP_TEST_RED   0xf80000
#define PANELCAP_TEST_GREEN 0x00fc00
#define PANELCAP_TEST_BLUE  0x0000f8
#define PANELCAP_TEST_ROWS  16
#define PANELCAP_TEST_Y     4
#define PANELCAP_TEST_H     2

static void
test_panelcap_full_pattern(void) {
    gfx_present_drain();
    gfx_mode_exit();
    const gfx_mode_request_t request = {.layout = GFX_LAYOUT_FULL_FB};
    TEST_ASSERT_EQUAL(GFX_LAYOUT_FULL_FB, gfx_mode_enter(&request)->layout);
    gfx_set_debug_overlay(false);
    gfx_set_leaf_overlay(false);
    gfx_set_interlace(false);
    draw_clear(RGB565BE(PANELCAP_TEST_BLUE));
    gfx_color_t* pixels = gfx_framebuffer();
    for (int i = 0; i < GFX_WIDTH * PANELCAP_TEST_ROWS; i++) {
        pixels[i] = i % 2 == 0 ? RGB565BE(PANELCAP_TEST_RED) : RGB565BE(PANELCAP_TEST_GREEN);
    }
    gfx_mark_dirty(0, 0, GFX_WIDTH, PANELCAP_TEST_ROWS);
    const int64_t before = gfx_get_bytes_sent();
    gfx_present();
    TEST_ASSERT_EQUAL(GFX_FRAMEBUFFER_BYTES, gfx_get_bytes_sent() - before);
}

static void
test_panelcap_partial_pattern(void) {
    dirty_all_rows_sent();
    const int64_t before = gfx_get_bytes_sent();
    draw_rect(0, PANELCAP_TEST_Y, GFX_WIDTH, PANELCAP_TEST_H, RGB565BE(PANELCAP_TEST_BLUE));
    gfx_present();
    TEST_ASSERT_EQUAL(GFX_WIDTH * PANELCAP_TEST_H * PANELCAP_PIXEL_BYTES, gfx_get_bytes_sent() - before);
}

void
suite_panelcap_pattern(void) {
    RUN_TEST(test_panelcap_full_pattern);
    RUN_TEST(test_panelcap_partial_pattern);
}

SUITE_REGISTER_ON_REQUEST(suite_panelcap_pattern);
#endif

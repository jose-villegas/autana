#include <stdlib.h>
#include <string.h>
#include "core/memory.h"
#include "gfx/present/capture/panelcap.h"
#include "suites.h"
#include "testkit/cleanup.h"
#include "unity.h"

#define TEST_PACKET_BYTES   31U
#define TEST_CASES          4U
#define TEST_WINDOW_SPACING 2U

static panelcap_t* capture;
static uint8_t* storage;

static void
release(void) {
    memory_free(storage);
    storage = NULL;
    memory_free(capture);
    capture = NULL;
}

static void
fixture(void) {
    suite_set_test_cleanup(release);
    capture = memory_alloc(sizeof *capture, MEMORY_APP);
    TEST_ASSERT_NOT_NULL(capture);
    storage = memory_alloc(PANELCAP_IMAGE_BYTES, MEMORY_APP);
    TEST_ASSERT_NOT_NULL(storage);
    panelcap_init(capture, storage, PANELCAP_FPS_DEFAULT);
}

typedef struct {
    uint8_t* bytes;
    size_t used;
} sink_t;

static void
collect(void* arg, const uint8_t* bytes, size_t size) {
    sink_t* sink = arg;
    memcpy(sink->bytes + sink->used, bytes, size);
    sink->used += size;
}

static void
test_packbits_roundtrips_and_bound(void) {
    const size_t counts[] = {1, PANELCAP_PACK_RUN_MAX, PANELCAP_PACK_RUN_MAX + 1, PANELCAP_WIDTH * PANELCAP_HEIGHT};
    for (unsigned c = 0; c < sizeof counts / sizeof counts[0]; c++) {
        size_t count = counts[c];
        uint8_t* raw = memory_alloc(count * PANELCAP_PIXEL_BYTES, MEMORY_APP);
        uint8_t* packed = memory_alloc(panelcap_pack_bound(count), MEMORY_APP);
        uint8_t* decoded = memory_alloc(count * PANELCAP_PIXEL_BYTES, MEMORY_APP);
        TEST_ASSERT_NOT_NULL(raw);
        TEST_ASSERT_NOT_NULL(packed);
        TEST_ASSERT_NOT_NULL(decoded);
        for (unsigned pattern = 0; pattern < TEST_CASES; pattern++) {
            for (size_t i = 0; i < count; i++) {
                unsigned value = pattern == 0 ? (unsigned)i : pattern == 1 ? 0 : pattern == 2 ? i % 2 : i / 3;
                bytes_write_u16(raw + i * PANELCAP_PIXEL_BYTES, value);
            }
            sink_t sink = {.bytes = packed};
            size_t encoded = panelcap_pack(raw, count, collect, &sink);
            TEST_ASSERT_EQUAL(encoded, sink.used);
            TEST_ASSERT_LESS_OR_EQUAL(panelcap_pack_bound(count), encoded);
            TEST_ASSERT_EQUAL(encoded, panelcap_pack(raw, count, NULL, NULL));
            size_t at = 0;
            size_t out = 0;
            while (at < encoded) {
                int head = (int8_t)packed[at++];
                size_t n = head >= 0 ? (size_t)head + 1 : (size_t)(1 - head);
                if (head == INT8_MIN) {
                    continue;
                }
                for (size_t i = 0; i < n; i++) {
                    TEST_ASSERT_LESS_THAN(count, out);
                    memcpy(decoded + out++ * PANELCAP_PIXEL_BYTES,
                           packed + at + (head >= 0 ? i * PANELCAP_PIXEL_BYTES : 0), PANELCAP_PIXEL_BYTES);
                }
                at += (head >= 0 ? n : 1) * PANELCAP_PIXEL_BYTES;
            }
            TEST_ASSERT_EQUAL(count, out);
            TEST_ASSERT_EQUAL_MEMORY(raw, decoded, count * PANELCAP_PIXEL_BYTES);
        }
        memory_free(raw);
        memory_free(packed);
        memory_free(decoded);
    }
}

static void
test_busy_sender_drops_whole_present(void) {
    fixture();
    const uint8_t pixel[] = {0x12, 0x34};
    TEST_ASSERT_TRUE(panelcap_begin(capture, 0));
    panelcap_window(capture, 0, 0, 1, 1, pixel);
    panelcap_commit(capture);
    TEST_ASSERT_FALSE(panelcap_begin(capture, PANELCAP_US_PER_SECOND));
    const uint8_t refused[] = {0xab, 0xcd};
    panelcap_window(capture, 0, 0, 1, 1, refused);
    panelcap_commit(capture);
    TEST_ASSERT_EQUAL_MEMORY(pixel, capture->storage, sizeof pixel);
    TEST_ASSERT_EQUAL(1, bytes_read_u32(panelcap_peek(capture)));
    TEST_ASSERT_EQUAL(1, capture->presents);
    TEST_ASSERT_EQUAL(1, capture->dropped);
    panelcap_release(capture);
    TEST_ASSERT_TRUE(panelcap_begin(capture, 2 * PANELCAP_US_PER_SECOND));
    panelcap_commit(capture);
    TEST_ASSERT_EQUAL(1, bytes_read_u16(panelcap_peek(capture) + PANELCAP_HEAD_BYTES - sizeof(uint16_t)));
}

static void
test_fps_gate_counts_dropped_presents(void) {
    fixture();
    TEST_ASSERT_TRUE(panelcap_begin(capture, 0));
    panelcap_commit(capture);
    panelcap_release(capture);
    TEST_ASSERT_FALSE(panelcap_begin(capture, 1));
    panelcap_commit(capture);
    TEST_ASSERT_NULL(panelcap_peek(capture));
    TEST_ASSERT_TRUE(panelcap_begin(capture, PANELCAP_US_PER_SECOND / PANELCAP_FPS_DEFAULT));
    panelcap_commit(capture);
    TEST_ASSERT_EQUAL(1, bytes_read_u16(panelcap_peek(capture) + PANELCAP_HEAD_BYTES - sizeof(uint16_t)));
}

static void
test_window_overflow_collapses_to_bounding_box(void) {
    fixture();
    const uint8_t pixel[] = {0x12, 0x34};
    TEST_ASSERT_TRUE(panelcap_begin(capture, 0));
    for (unsigned i = 0; i <= PANELCAP_WINDOWS_MAX; i++) {
        unsigned x = (i * TEST_WINDOW_SPACING) % PANELCAP_WIDTH;
        unsigned y = (i * TEST_WINDOW_SPACING) / PANELCAP_WIDTH;
        panelcap_window(capture, x, y, x + 1, y + 1, pixel);
    }
    TEST_ASSERT_TRUE(capture->admitted);
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
    TEST_ASSERT_LESS_OR_EQUAL(TEST_PACKET_BYTES, size);
    TEST_ASSERT_EQUAL(1, bytes_read_u32(bytes));
    TEST_ASSERT_EQUAL(joined->part++, bytes_read_u16(bytes + sizeof(uint32_t)));
    joined->last = bytes[PANELCAP_PART_HEAD_BYTES - 1];
    collect(&joined->sink, bytes + PANELCAP_PART_HEAD_BYTES, size - PANELCAP_PART_HEAD_BYTES);
}

static void
test_continuation_rejoins_encoded_present(void) {
    fixture();
    const uint8_t pixels[] = {0x12, 0x34};
    TEST_ASSERT_TRUE(panelcap_begin(capture, 0));
    panelcap_window(capture, 0, 0, 1, 1, pixels);
    panelcap_commit(capture);
    uint8_t packet[TEST_PACKET_BYTES];
    uint8_t result[PANELCAP_HEAD_BYTES + PANELCAP_WINDOW_BYTES + 1 + sizeof pixels];
    join_t joined = {.sink = {.bytes = result}};
    panelcap_send(capture, packet, sizeof packet, join, &joined);
    TEST_ASSERT_TRUE(joined.last);
    TEST_ASSERT_EQUAL(2, joined.part);
    TEST_ASSERT_EQUAL(sizeof result, joined.sink.used);
    TEST_ASSERT_EQUAL(1 + sizeof pixels,
                      bytes_read_u32(result + PANELCAP_HEAD_BYTES + PANELCAP_COORDS * sizeof(uint16_t)));
    TEST_ASSERT_EQUAL_MEMORY(pixels, result + PANELCAP_HEAD_BYTES + PANELCAP_WINDOW_BYTES + 1, sizeof pixels);
}

static void
test_overlapping_windows_keep_last_pixel(void) {
    fixture();
    const uint8_t first[] = {0x12, 0x34};
    const uint8_t last[] = {0x56, 0x78};
    TEST_ASSERT_TRUE(panelcap_begin(capture, 0));
    panelcap_window(capture, 0, 0, 1, 1, first);
    panelcap_window(capture, 0, 0, 1, 1, last);
    panelcap_commit(capture);
    uint8_t packet[TEST_PACKET_BYTES];
    uint8_t result[PANELCAP_HEAD_BYTES + 2 * (PANELCAP_WINDOW_BYTES + 1 + sizeof first)];
    join_t joined = {.sink = {.bytes = result}};
    panelcap_send(capture, packet, sizeof packet, join, &joined);
    TEST_ASSERT_EQUAL(1, bytes_read_u16(result + sizeof(uint32_t) + sizeof(uint64_t)));
    TEST_ASSERT_EQUAL_MEMORY(last, result + PANELCAP_HEAD_BYTES + PANELCAP_WINDOW_BYTES + 1, sizeof last);
}

static void
test_image_rows_and_refusal_are_whole_present(void) {
    fixture();
    const uint8_t rows[] = {0x12, 0x34, 0x56, 0x78};
    TEST_ASSERT_TRUE(panelcap_begin(capture, 0));
    panelcap_window(capture, 1, 1, 2, 3, rows);
    TEST_ASSERT_EQUAL_MEMORY(rows, capture->storage + (PANELCAP_WIDTH + 1) * PANELCAP_PIXEL_BYTES,
                             PANELCAP_PIXEL_BYTES);
    TEST_ASSERT_EQUAL_MEMORY(rows + PANELCAP_PIXEL_BYTES,
                             capture->storage + (2 * PANELCAP_WIDTH + 1) * PANELCAP_PIXEL_BYTES, PANELCAP_PIXEL_BYTES);
    panelcap_window(capture, 0, 0, PANELCAP_WIDTH + 1, 1, rows);
    panelcap_window(capture, 0, 0, 1, 1, rows);
    panelcap_commit(capture);
    TEST_ASSERT_NULL(panelcap_peek(capture));
    TEST_ASSERT_EQUAL(1, capture->dropped);
    TEST_ASSERT_TRUE(panelcap_begin(capture, PANELCAP_US_PER_SECOND));
    panelcap_commit(capture);
    TEST_ASSERT_EQUAL(1, bytes_read_u16(panelcap_peek(capture) + PANELCAP_HEAD_BYTES - sizeof(uint16_t)));
}

static void
test_wire_fields_are_little_endian(void) {
    fixture();
    const uint64_t device_us = UINT64_C(0x0102030405060708);
    const uint32_t previous_seq = UINT32_C(0x44332210);
    const uint8_t expected[] = {0x11, 0x22, 0x33, 0x44, 8, 7, 6, 5, 4, 3, 2, 1, 0, 0, 0, 0};
    capture->seq = previous_seq;
    TEST_ASSERT_TRUE(panelcap_begin(capture, device_us));
    panelcap_commit(capture);
    TEST_ASSERT_EQUAL_MEMORY(expected, panelcap_peek(capture), sizeof expected);
}

void
suite_panelcap(void) {
    RUN_TEST(test_packbits_roundtrips_and_bound);
    RUN_TEST(test_busy_sender_drops_whole_present);
    RUN_TEST(test_fps_gate_counts_dropped_presents);
    RUN_TEST(test_window_overflow_collapses_to_bounding_box);
    RUN_TEST(test_continuation_rejoins_encoded_present);
    RUN_TEST(test_overlapping_windows_keep_last_pixel);
    RUN_TEST(test_image_rows_and_refusal_are_whole_present);
    RUN_TEST(test_wire_fields_are_little_endian);
}

SUITE_REGISTER(suite_panelcap);

#ifdef DEVICE_BUILD
#include "draw/draw.h"
#include "gfx/gfx.h"
#include "gfx/present/gfx_debug.h"
#include "gfx/present/gfx_internal.h"
#include "gfx/present/gfx_present.h"
#include "image/pixel/rgb565.h"

#define PANELCAP_TEST_RGB 0x204060

static void
test_panelcap_known_pattern_present(void) {
    gfx_present_drain();
    gfx_mode_exit();
    const gfx_mode_request_t request = {.layout = GFX_LAYOUT_FULL_FB};
    TEST_ASSERT_EQUAL(GFX_LAYOUT_FULL_FB, gfx_mode_enter(&request)->layout);
    gfx_set_debug_overlay(false);
    gfx_set_leaf_overlay(false);
    draw_clear(RGB565BE(PANELCAP_TEST_RGB));
    gfx_present_raw_full_frame_for_test();
}

void
suite_panelcap_pattern(void) {
    RUN_TEST(test_panelcap_known_pattern_present);
}

SUITE_REGISTER_ON_REQUEST(suite_panelcap_pattern);
#endif

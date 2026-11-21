#include <stdlib.h>
#include <string.h>
#include "gfx/present/capture/panelcap.h"
#include "suites.h"
#include "unity.h"

#define TEST_PACKET_BYTES 31U
#define TEST_CASES        4U

static panelcap_ring_t ring;
static uint8_t* storage;

static void
fixture(void) {
    storage = malloc(PANELCAP_RING_BYTES);
    TEST_ASSERT_NOT_NULL(storage);
    panelcap_ring_init(&ring, storage, PANELCAP_FPS_DEFAULT);
}

static void
release(void) {
    free(storage);
    storage = NULL;
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
        uint8_t* raw = malloc(count * PANELCAP_PIXEL_BYTES);
        uint8_t* packed = malloc(panelcap_pack_bound(count));
        uint8_t* decoded = malloc(count * PANELCAP_PIXEL_BYTES);
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
        free(raw);
        free(packed);
        free(decoded);
    }
}

static void
test_admission_full_and_fps(void) {
    fixture();
    TEST_ASSERT_TRUE(panelcap_begin(&ring, 0));
    panelcap_commit(&ring);
    TEST_ASSERT_FALSE(panelcap_begin(&ring, 1));
    TEST_ASSERT_TRUE(panelcap_begin(&ring, PANELCAP_US_PER_SECOND));
    panelcap_commit(&ring);
    TEST_ASSERT_FALSE(panelcap_begin(&ring, 2 * PANELCAP_US_PER_SECOND));
    TEST_ASSERT_EQUAL(2, ring.dropped);
    TEST_ASSERT_EQUAL(1, bytes_read_u32(panelcap_peek(&ring)));
    panelcap_release(&ring);
    TEST_ASSERT_TRUE(panelcap_begin(&ring, 3 * PANELCAP_US_PER_SECOND));
    panelcap_commit(&ring);
    panelcap_release(&ring);
    const uint8_t* head = panelcap_peek(&ring);
    TEST_ASSERT_EQUAL(5, bytes_read_u32(head));
    TEST_ASSERT_EQUAL(1, bytes_read_u16(head + PANELCAP_HEAD_BYTES - sizeof(uint16_t)));
    release();
}

static void
test_overrun_abandons_whole_present(void) {
    fixture();
    uint8_t* pixels = calloc(PANELCAP_WIDTH * PANELCAP_HEIGHT, PANELCAP_PIXEL_BYTES);
    TEST_ASSERT_NOT_NULL(pixels);
    TEST_ASSERT_TRUE(panelcap_begin(&ring, 0));
    panelcap_window(&ring, 0, 0, PANELCAP_WIDTH, PANELCAP_HEIGHT, pixels);
    panelcap_window(&ring, 0, 0, PANELCAP_WIDTH, PANELCAP_HEIGHT, pixels);
    TEST_ASSERT_TRUE(ring.admitted);
    panelcap_window(&ring, 0, 0, PANELCAP_WIDTH, PANELCAP_HEIGHT, pixels);
    panelcap_commit(&ring);
    TEST_ASSERT_NULL(panelcap_peek(&ring));
    TEST_ASSERT_EQUAL(0, ring.used);
    TEST_ASSERT_EQUAL(1, ring.dropped);
    TEST_ASSERT_TRUE(panelcap_begin(&ring, PANELCAP_US_PER_SECOND));
    panelcap_window(&ring, 0, 0, 1, 1, pixels);
    panelcap_commit(&ring);
    TEST_ASSERT_EQUAL(1, bytes_read_u16(panelcap_peek(&ring) + sizeof(uint32_t) + sizeof(uint64_t)));
    free(pixels);
    release();
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
    TEST_ASSERT_TRUE(panelcap_begin(&ring, 0));
    panelcap_window(&ring, 0, 0, 1, 1, pixels);
    panelcap_commit(&ring);
    uint8_t packet[TEST_PACKET_BYTES];
    uint8_t result[PANELCAP_HEAD_BYTES + PANELCAP_WINDOW_BYTES + 1 + sizeof pixels];
    join_t joined = {.sink = {.bytes = result}};
    panelcap_send(panelcap_peek(&ring), packet, sizeof packet, join, &joined);
    TEST_ASSERT_TRUE(joined.last);
    TEST_ASSERT_EQUAL(2, joined.part);
    TEST_ASSERT_EQUAL(sizeof result, joined.sink.used);
    TEST_ASSERT_EQUAL(1 + sizeof pixels,
                      bytes_read_u32(result + PANELCAP_HEAD_BYTES + PANELCAP_COORDS * sizeof(uint16_t)));
    TEST_ASSERT_EQUAL_MEMORY(pixels, result + PANELCAP_HEAD_BYTES + PANELCAP_WINDOW_BYTES + 1, sizeof pixels);
    release();
}

static void
test_wire_fields_are_little_endian(void) {
    fixture();
    const uint64_t device_us = UINT64_C(0x0102030405060708);
    const uint32_t previous_seq = UINT32_C(0x44332210);
    const uint8_t expected[] = {0x11, 0x22, 0x33, 0x44, 8, 7, 6, 5, 4, 3, 2, 1, 0, 0, 0, 0};
    ring.seq = previous_seq;
    TEST_ASSERT_TRUE(panelcap_begin(&ring, device_us));
    panelcap_commit(&ring);
    TEST_ASSERT_EQUAL_MEMORY(expected, panelcap_peek(&ring), sizeof expected);
    release();
}

void
run_panelcap_suite(void) {
    RUN_TEST(test_packbits_roundtrips_and_bound);
    RUN_TEST(test_admission_full_and_fps);
    RUN_TEST(test_overrun_abandons_whole_present);
    RUN_TEST(test_continuation_rejoins_encoded_present);
    RUN_TEST(test_wire_fields_are_little_endian);
}

SUITE_REGISTER(run_panelcap_suite);

#ifdef DEVICE_BUILD
#include "draw/draw.h"
#include "gfx/gfx.h"
#include "gfx/present/gfx_debug.h"
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
run_panelcap_pattern_suite(void) {
    RUN_TEST(test_panelcap_known_pattern_present);
}

SUITE_REGISTER_ON_REQUEST(run_panelcap_pattern_suite);
#endif

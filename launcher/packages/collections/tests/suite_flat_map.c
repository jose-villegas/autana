#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "testkit/suite.h"
#include "unity.h"

#include "collections/flat_map.h"

/* A name before the key puts the key at an odd offset, as a packed pack row would. */
#define ROW_NAME   3U
#define ROW_STRIDE (ROW_NAME + sizeof(uint32_t) + 2U)
#define SWEEP_MAX  9U

static uint8_t*
rows_of(const uint32_t* keys, size_t count) {
    uint8_t* rows = calloc(count == 0 ? 1 : count, ROW_STRIDE);
    TEST_ASSERT_NOT_NULL(rows);
    for (size_t i = 0; i < count; i++) {
        uint8_t* key = rows + (i * ROW_STRIDE) + ROW_NAME;
        for (size_t b = 0; b < sizeof(uint32_t); b++) {
            key[b] = (uint8_t)(keys[i] >> (8U * b));
        }
    }
    return rows;
}

static void
expect_refused(const uint32_t* keys, size_t count, flat_map_status_t status, size_t at) {
    uint8_t* rows = rows_of(keys, count);
    flat_map_t map;
    size_t clash = 0;
    TEST_ASSERT_EQUAL(status, flat_map_open(&map, rows, count, ROW_STRIDE, ROW_NAME, &clash));
    TEST_ASSERT_EQUAL_size_t(at, clash);
    TEST_ASSERT_EQUAL_size_t(FLAT_MAP_MISSING, flat_map_find(&map, keys[0]));
    TEST_ASSERT_EQUAL(status, flat_map_open(&map, rows, count, ROW_STRIDE, ROW_NAME, NULL));
    TEST_ASSERT_EQUAL_size_t(FLAT_MAP_MISSING, flat_map_find(&map, keys[0]));
    free(rows);
}

static void
test_flat_map_finds_every_row_and_misses_between_them_at_every_size(void) {
    for (size_t count = 0; count <= SWEEP_MAX; count++) {
        uint32_t keys[SWEEP_MAX];
        for (size_t i = 0; i < count; i++) {
            keys[i] = 10U + (10U * (uint32_t)i);
        }
        uint8_t* rows = rows_of(keys, count);
        flat_map_t map;
        TEST_ASSERT_EQUAL(FLAT_MAP_OK, flat_map_open(&map, rows, count, ROW_STRIDE, ROW_NAME, NULL));
        TEST_ASSERT_EQUAL_size_t(FLAT_MAP_MISSING, flat_map_find(&map, 0U));
        TEST_ASSERT_EQUAL_size_t(FLAT_MAP_MISSING, flat_map_find(&map, UINT32_MAX));
        for (size_t i = 0; i < count; i++) {
            TEST_ASSERT_EQUAL_size_t(i, flat_map_find(&map, keys[i]));
            TEST_ASSERT_EQUAL_size_t(FLAT_MAP_MISSING, flat_map_find(&map, keys[i] - 1U));
            TEST_ASSERT_EQUAL_size_t(FLAT_MAP_MISSING, flat_map_find(&map, keys[i] + 1U));
        }
        free(rows);
    }
}

static void
test_flat_map_orders_keys_unsigned_from_zero_to_the_top(void) {
    static const uint32_t keys[] = {0U, 9U, 0x7FFFFFFFU, 0x80000000U, UINT32_MAX};

    enum { COUNT = sizeof keys / sizeof keys[0] };

    uint8_t* rows = rows_of(keys, COUNT);
    flat_map_t map;
    TEST_ASSERT_EQUAL(FLAT_MAP_OK, flat_map_open(&map, rows, COUNT, ROW_STRIDE, ROW_NAME, NULL));
    for (size_t i = 0; i < COUNT; i++) {
        TEST_ASSERT_EQUAL_size_t(i, flat_map_find(&map, keys[i]));
    }
    TEST_ASSERT_EQUAL_PTR(rows + (2U * ROW_STRIDE), flat_map_row(&map, 2));
    free(rows);
}

static void
test_flat_map_refuses_the_first_shared_or_falling_key_at_its_row(void) {
    static const uint32_t shared_first[] = {5U, 5U};
    static const uint32_t falling_first[] = {5U, 4U};
    static const uint32_t shared_later[] = {1U, 5U, 5U, 8U};
    static const uint32_t falling_later[] = {1U, 5U, 8U, 4U};
    static const uint32_t shared_then_falling[] = {1U, 3U, 3U, 2U};
    expect_refused(shared_first, 2, FLAT_MAP_DUPLICATE, 1);
    expect_refused(falling_first, 2, FLAT_MAP_UNSORTED, 1);
    expect_refused(shared_later, 4, FLAT_MAP_DUPLICATE, 2);
    expect_refused(falling_later, 4, FLAT_MAP_UNSORTED, 3);
    expect_refused(shared_then_falling, 4, FLAT_MAP_DUPLICATE, 2);
}

static void
test_bytes_read_little_endian_at_any_alignment(void) {
    static const uint8_t stored[] = {0xEEU, 0x01U, 0x02U, 0x03U, 0x84U};
    TEST_ASSERT_EQUAL_HEX16(0x0201U, bytes_read_u16(stored + 1));
    TEST_ASSERT_EQUAL_HEX32(0x84030201U, bytes_read_u32(stored + 1));
}

void
run_flat_map_suite(void) {
    RUN_TEST(test_flat_map_finds_every_row_and_misses_between_them_at_every_size);
    RUN_TEST(test_flat_map_orders_keys_unsigned_from_zero_to_the_top);
    RUN_TEST(test_flat_map_refuses_the_first_shared_or_falling_key_at_its_row);
    RUN_TEST(test_bytes_read_little_endian_at_any_alignment);
}

SUITE_REGISTER(run_flat_map_suite);

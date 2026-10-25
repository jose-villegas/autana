#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "testkit/suite.h"
#include "unity.h"

#include "collections/flat_map.h"

/* A name before the key puts the key at an odd offset, as a packed pack row would. */
#define ROW_NAME   3U
#define ROW_STRIDE (ROW_NAME + sizeof(uint32_t) + 2U)

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
test_flat_map_finds_every_row_and_misses_between_them(void) {
    static const uint32_t keys[] = {2U, 9U, 0x00010000U, 0x7FFFFFFFU, 0x80000000U, UINT32_MAX};

    enum { COUNT = sizeof keys / sizeof keys[0] };

    uint8_t* rows = rows_of(keys, COUNT);
    flat_map_t map;
    TEST_ASSERT_EQUAL(FLAT_MAP_OK, flat_map_open(&map, rows, COUNT, ROW_STRIDE, ROW_NAME, NULL));
    for (size_t i = 0; i < COUNT; i++) {
        TEST_ASSERT_EQUAL_size_t(i, flat_map_find(&map, keys[i]));
        if (keys[i] > 0U && (i == 0 || keys[i] - 1U != keys[i - 1])) {
            TEST_ASSERT_EQUAL_size_t(FLAT_MAP_MISSING, flat_map_find(&map, keys[i] - 1U));
        }
    }
    free(rows);
}

static void
test_flat_map_refuses_a_shared_or_falling_key_at_its_row(void) {
    static const uint32_t shared[] = {1U, 5U, 5U, 8U};
    static const uint32_t falling[] = {1U, 5U, 8U, 4U};
    uint8_t* rows = rows_of(shared, 4);
    flat_map_t map;
    size_t clash = 0;
    TEST_ASSERT_EQUAL(FLAT_MAP_SHARED, flat_map_open(&map, rows, 4, ROW_STRIDE, ROW_NAME, &clash));
    TEST_ASSERT_EQUAL_size_t(2, clash);
    TEST_ASSERT_EQUAL_size_t(FLAT_MAP_MISSING, flat_map_find(&map, 1U));
    free(rows);

    rows = rows_of(falling, 4);
    TEST_ASSERT_EQUAL(FLAT_MAP_UNSORTED, flat_map_open(&map, rows, 4, ROW_STRIDE, ROW_NAME, &clash));
    TEST_ASSERT_EQUAL_size_t(3, clash);
    TEST_ASSERT_EQUAL_size_t(FLAT_MAP_MISSING, flat_map_find(&map, 8U));
    free(rows);
}

static void
test_flat_map_holds_no_rows_or_one(void) {
    static const uint32_t one[] = {42U};
    uint8_t* rows = rows_of(one, 1);
    flat_map_t map;
    TEST_ASSERT_EQUAL(FLAT_MAP_OK, flat_map_open(&map, rows, 0, ROW_STRIDE, ROW_NAME, NULL));
    TEST_ASSERT_EQUAL_size_t(FLAT_MAP_MISSING, flat_map_find(&map, 42U));
    TEST_ASSERT_EQUAL(FLAT_MAP_OK, flat_map_open(&map, rows, 1, ROW_STRIDE, ROW_NAME, NULL));
    TEST_ASSERT_EQUAL_size_t(0, flat_map_find(&map, 42U));
    TEST_ASSERT_EQUAL_size_t(FLAT_MAP_MISSING, flat_map_find(&map, 41U));
    TEST_ASSERT_EQUAL_size_t(FLAT_MAP_MISSING, flat_map_find(&map, 43U));
    free(rows);
}

void
run_flat_map_suite(void) {
    RUN_TEST(test_flat_map_finds_every_row_and_misses_between_them);
    RUN_TEST(test_flat_map_refuses_a_shared_or_falling_key_at_its_row);
    RUN_TEST(test_flat_map_holds_no_rows_or_one);
}

SUITE_REGISTER(run_flat_map_suite);

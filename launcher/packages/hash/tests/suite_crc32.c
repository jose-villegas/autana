#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "testkit/cleanup.h"
#include "testkit/suite.h"
#include "unity.h"

#include "hash/crc32.h"

/* The CRC-32 check value: every catalogue of CRC parameters lists it for "123456789". */
static const char check_input[] = "123456789";
#define CRC32_CHECK 0xCBF43926U

static void
test_crc32_of_the_check_string(void) {
    TEST_ASSERT_EQUAL_HEX32(CRC32_CHECK, crc32_update(0, check_input, sizeof check_input - 1));
}

static void
test_crc32_of_nothing_is_zero(void) {
    TEST_ASSERT_EQUAL_HEX32(0, crc32_update(0, NULL, 0));
}

static void
test_crc32_continued_over_pieces_equals_one_pass(void) {
    const size_t length = sizeof check_input - 1;
    for (size_t cut = 0; cut <= length; cut++) {
        const uint32_t first = crc32_update(0, check_input, cut);
        TEST_ASSERT_EQUAL_HEX32(CRC32_CHECK, crc32_update(first, check_input + cut, length - cut));
    }
}

#define CRC32_TEST_CAPACITY       8192
#define CRC32_TEST_ALIGNMENT      4
#define CRC32_TEST_RANDOM_LENGTHS 12

typedef uint32_t (*crc32_fn_t)(uint32_t, const void*, size_t);

static uint8_t* active_buffer;

static void
release_buffer(void) {
    free(active_buffer);
    active_buffer = NULL;
}

static void
test_crc32_candidates_match_reference(void) {
    static const crc32_fn_t candidates[] = {
        crc32_update,
        crc32_update_slice4,
#ifdef ESP_PLATFORM
        crc32_update_rom,
#endif
    };
    static const uint32_t seeds[] = {0, UINT32_MAX, 0x12345678U, 0x9E3779B9U};
    static const size_t lengths[] = {0, 1, 3, 7, 64, 4096, CRC32_TEST_CAPACITY};
    static const size_t chunks[] = {1, 2, 3, 7, 31, 64, 4096};
    uint8_t* buffer = malloc(CRC32_TEST_CAPACITY + CRC32_TEST_ALIGNMENT);
    active_buffer = buffer;
    suite_set_test_cleanup(release_buffer);
    TEST_ASSERT_NOT_NULL(buffer);
    for (size_t seed = 0; seed < sizeof seeds / sizeof *seeds; seed++) {
        srand(seeds[seed]);
        for (size_t i = 0; i < CRC32_TEST_CAPACITY + CRC32_TEST_ALIGNMENT; i++) {
            buffer[i] = (uint8_t)rand();
        }
        for (size_t trial = 0; trial < sizeof lengths / sizeof *lengths + CRC32_TEST_RANDOM_LENGTHS; trial++) {
            const size_t size = trial < sizeof lengths / sizeof *lengths ? lengths[trial]
                                                                         : (uint32_t)rand() % (CRC32_TEST_CAPACITY + 1);
            const uint32_t initial = trial < sizeof lengths / sizeof *lengths ? seeds[seed] : (uint32_t)rand();
            for (size_t offset = 0; offset < CRC32_TEST_ALIGNMENT; offset++) {
                const uint8_t* data = buffer + offset;
                const uint32_t expected = crc32_update_nibble(initial, data, size);
                for (size_t candidate = 0; candidate < sizeof candidates / sizeof *candidates; candidate++) {
                    const crc32_fn_t update = candidates[candidate];
                    TEST_ASSERT_EQUAL_HEX32(initial, update(initial, NULL, 0));
                    TEST_ASSERT_EQUAL_HEX32(expected, update(initial, data, size));
                    for (size_t chunk = 0; chunk < sizeof chunks / sizeof *chunks; chunk++) {
                        uint32_t crc = initial;
                        for (size_t start = 0; start < size;) {
                            const size_t count = size - start < chunks[chunk] ? size - start : chunks[chunk];
                            crc = update(crc, data + start, count);
                            start += count;
                        }
                        TEST_ASSERT_EQUAL_HEX32(expected, crc);
                        const size_t cut = chunks[chunk] < size ? chunks[chunk] : size;
                        crc = update(initial, data, cut);
                        TEST_ASSERT_EQUAL_HEX32(expected, update(crc, data + cut, size - cut));
                    }
                }
            }
        }
    }
    printf("FASTERROR name=crc32_update type=bits max=%d unit=bit method=sweep\n", HASH_CRC32_BIT_LIMIT);
}

void
run_crc32_suite(void) {
    RUN_TEST(test_crc32_candidates_match_reference);
    RUN_TEST(test_crc32_of_the_check_string);
    RUN_TEST(test_crc32_of_nothing_is_zero);
    RUN_TEST(test_crc32_continued_over_pieces_equals_one_pass);
}

SUITE_REGISTER(run_crc32_suite);

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "suites.h"
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

void
run_crc32_suite(void) {
    RUN_TEST(test_crc32_of_the_check_string);
    RUN_TEST(test_crc32_of_nothing_is_zero);
    RUN_TEST(test_crc32_continued_over_pieces_equals_one_pass);
}

SUITE_REGISTER(run_crc32_suite);

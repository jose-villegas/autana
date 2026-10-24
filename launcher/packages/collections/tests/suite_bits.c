#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "suites.h"
#include "unity.h"

#include "collections/bits.h"

enum { FLAGS = 70 };

static void
test_bits_counts_round_up_to_whole_storage(void) {
    TEST_ASSERT_EQUAL_size_t(0, BITS8_BYTES(0));
    TEST_ASSERT_EQUAL_size_t(1, BITS8_BYTES(1));
    TEST_ASSERT_EQUAL_size_t(1, BITS8_BYTES(8));
    TEST_ASSERT_EQUAL_size_t(2, BITS8_BYTES(9));
    TEST_ASSERT_EQUAL_size_t(1, BITS32_WORDS(32));
    TEST_ASSERT_EQUAL_size_t(2, BITS32_WORDS(33));
}

static void
test_bits8_sets_only_its_own_flag_bit_zero_first(void) {
    uint8_t bits[BITS8_BYTES(FLAGS)] = {0};
    bits8_set(bits, 0);
    bits8_set(bits, 9);
    bits8_set(bits, FLAGS - 1);
    TEST_ASSERT_EQUAL_HEX8(0x01, bits[0]);
    TEST_ASSERT_EQUAL_HEX8(0x02, bits[1]);
    for (size_t i = 0; i < FLAGS; i++) {
        TEST_ASSERT_EQUAL(i == 0 || i == 9 || i == FLAGS - 1, bits8_get(bits, i));
    }
}

static void
test_bits32_sets_only_its_own_flag_bit_zero_first(void) {
    uint32_t bits[BITS32_WORDS(FLAGS)] = {0};
    bits32_set(bits, 31);
    bits32_set(bits, 32);
    TEST_ASSERT_EQUAL_HEX32(UINT32_C(1) << 31, bits[0]);
    TEST_ASSERT_EQUAL_HEX32(1, bits[1]);
    for (size_t i = 0; i < FLAGS; i++) {
        TEST_ASSERT_EQUAL(i == 31 || i == 32, bits32_get(bits, i));
    }
}

void
run_bits_suite(void) {
    RUN_TEST(test_bits_counts_round_up_to_whole_storage);
    RUN_TEST(test_bits8_sets_only_its_own_flag_bit_zero_first);
    RUN_TEST(test_bits32_sets_only_its_own_flag_bit_zero_first);
}

SUITE_REGISTER(run_bits_suite);

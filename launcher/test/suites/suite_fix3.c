/*
 * Portable suite: render/fix3.h. Two quirks of the maths are kept because
 * pinned renders were drawn with them; a test each, so a "corrected" sine
 * table or wrap fails here and not as a changed picture.
 */

#include "suites.h"
#include "unity.h"

#include "render/fix3.h"

static void
test_the_sine_table_peaks_at_510_not_one(void) {
    TEST_ASSERT_EQUAL_INT(510, fix3_sin(FIX3_ONE / 4));
    TEST_ASSERT_EQUAL_INT(-510, fix3_sin(3 * FIX3_ONE / 4));
}

static void
test_wrap_of_a_negative_value_comes_out_one_short(void) {
    TEST_ASSERT_EQUAL_INT(510, fix3_wrap(-1, FIX3_ONE));
    TEST_ASSERT_EQUAL_INT(3, fix3_wrap(3, FIX3_ONE));
}

void
run_fix3_suite(void) {
    RUN_TEST(test_the_sine_table_peaks_at_510_not_one);
    RUN_TEST(test_wrap_of_a_negative_value_comes_out_one_short);
}

SUITE_REGISTER(run_fix3_suite);

#include <stdlib.h>

#include "suites.h"
#include "unity.h"

#include "../c_style_examples/clamp.c"

#include "../c_style_examples/acquire_all.c"

static void
fixture(void) {}

static void
test_clamp_preserves_values_below_the_limit(void) {
    fixture();
    TEST_ASSERT_EQUAL_INT(3, clamp_px(3, 4));
    TEST_ASSERT_EQUAL_INT(4, clamp_px(4, 4));
    TEST_ASSERT_EQUAL_INT(4, clamp_px(5, 4));
}

static void
test_acquiring_both_buffers_leaves_them_owned_by_the_caller(void) {
    fixture();
    uint8_t* a = NULL;
    uint8_t* b = NULL;
    TEST_ASSERT_TRUE(acquire_all(sizeof *a, &a, &b));
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_NOT_NULL(b);
    *a = UINT8_MAX;
    *b = *a;
    TEST_ASSERT_EQUAL_UINT8(UINT8_MAX, *b);
    free(a);
    free(b);
}

void
run_c_style_examples_suite(void) {
    RUN_TEST(test_clamp_preserves_values_below_the_limit);
    RUN_TEST(test_acquiring_both_buffers_leaves_them_owned_by_the_caller);
}

SUITE_REGISTER(run_c_style_examples_suite);

#include "suites.h"
#include "unity.h"

#include "app_memory.h"

static void
test_no_loss_within_allocator_tolerance_is_not_a_leak(void) {
    TEST_ASSERT_EQUAL_UINT(0, app_internal_memory_leaked_bytes(1000, 1000));
    TEST_ASSERT_EQUAL_UINT(0, app_internal_memory_leaked_bytes(1000, 1000 - APP_INTERNAL_MEMORY_TOLERANCE));
}

static void
test_a_loss_past_allocator_tolerance_is_reported_in_full(void) {
    TEST_ASSERT_EQUAL_UINT(65, app_internal_memory_leaked_bytes(1000, 935));
}

static void
test_more_free_memory_is_not_a_leak(void) {
    TEST_ASSERT_EQUAL_UINT(0, app_internal_memory_leaked_bytes(1000, 1001));
}

void
run_app_memory_suite(void) {
    RUN_TEST(test_no_loss_within_allocator_tolerance_is_not_a_leak);
    RUN_TEST(test_a_loss_past_allocator_tolerance_is_reported_in_full);
    RUN_TEST(test_more_free_memory_is_not_a_leak);
}

SUITE_REGISTER(run_app_memory_suite);

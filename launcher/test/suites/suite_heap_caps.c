/*
 * Portable suite: heap_arena.c's heap_caps_* pools. Internal and PSRAM are
 * independent budgets here the same way MALLOC_CAP_INTERNAL/_SPIRAM keep
 * them independent on the board - an allocation tagged for one must never
 * draw from the other, and one exceeding its own pool must fail exactly
 * like the device would, not spill over.
 */

#include "stubs/esp_heap_caps.h"

#include "suites.h"
#include "unity.h"

static void
test_an_internal_allocation_that_fits_succeeds(void) {
    void* p = heap_caps_malloc(64, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    TEST_ASSERT_NOT_NULL(p);
    heap_caps_free(p);
}

static void
test_an_internal_allocation_past_the_free_budget_fails(void) {
    const size_t free_now = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    void* p = heap_caps_malloc(free_now + 1, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    TEST_ASSERT_NULL(p);
}

static void
test_an_internal_request_never_spills_into_psram(void) {
    const size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t psram_free_before = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    void* p = heap_caps_malloc(internal_free + 1, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    TEST_ASSERT_NULL(p);
    TEST_ASSERT_EQUAL_UINT(psram_free_before, heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
}

static void
test_a_psram_allocation_does_not_consume_internal_budget(void) {
    const size_t internal_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    void* p = heap_caps_malloc(1024 * 1024, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQUAL_UINT(internal_before, heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));

    heap_caps_free(p);
}

static void
test_a_psram_allocation_past_its_own_budget_fails(void) {
    const size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    void* p = heap_caps_malloc(psram_free + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    TEST_ASSERT_NULL(p);
}

void
suite_heap_caps(void) {
    RUN_TEST(test_an_internal_allocation_that_fits_succeeds);
    RUN_TEST(test_an_internal_allocation_past_the_free_budget_fails);
    RUN_TEST(test_an_internal_request_never_spills_into_psram);
    RUN_TEST(test_a_psram_allocation_does_not_consume_internal_budget);
    RUN_TEST(test_a_psram_allocation_past_its_own_budget_fails);
}

SUITE_REGISTER(suite_heap_caps)

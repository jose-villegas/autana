#include "suites.h"
#include "unity.h"

#include <stdint.h>

#include "app_arena.h"

#if defined(HOST_HEAP_ARENA) || defined(DEVICE_BUILD)
#include "esp_heap_caps.h"
#endif
#ifdef HOST_HEAP_ARENA
#include <stdlib.h>
#endif
#ifdef DEVICE_BUILD
#include "esp_memory_utils.h"
#include "esp_psram.h"
#endif

/* On the board this resets the arena under the running app, which is safe
 * only because the app that runs the suites takes nothing from it. */
static void
fixture(void) {
    app_arena_reset();
}

static void
test_every_power_of_two_alignment_is_honoured(void) {
    fixture();
    for (size_t align = 1; align <= 4096; align *= 2) {
        TEST_ASSERT_NOT_NULL(app_arena_take(1, 1)); /* knocks the offset off any boundary */
        const uintptr_t p = (uintptr_t)app_arena_take(3, align);
        TEST_ASSERT_NOT_EQUAL(0, p);
        TEST_ASSERT_EQUAL_UINT(0, p % align);
    }
}

static void
test_consecutive_blocks_do_not_overlap(void) {
    fixture();
    unsigned char* a = app_arena_take(100, 1);
    unsigned char* b = app_arena_take(100, 1);
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_NOT_NULL(b);
    TEST_ASSERT_TRUE(b >= a + 100);
    TEST_ASSERT_EQUAL_UINT(200, app_arena_used());
}

static void
test_the_whole_block_can_be_taken_at_once(void) {
    fixture();
    TEST_ASSERT_NOT_NULL(app_arena_take(APP_ARENA_BYTES, 1));
    TEST_ASSERT_NULL(app_arena_take(1, 1));
}

static void
test_exhaustion_refuses_without_taking_and_the_arena_stays_usable(void) {
    fixture();
    TEST_ASSERT_NOT_NULL(app_arena_take(APP_ARENA_BYTES - 64, 1));
    const size_t used = app_arena_used();

    TEST_ASSERT_NULL(app_arena_take(65, 1));
    TEST_ASSERT_NULL(app_arena_take(SIZE_MAX, 1));
    TEST_ASSERT_EQUAL_UINT(used, app_arena_used());

    TEST_ASSERT_NOT_NULL(app_arena_take(64, 1));
}

static uintptr_t
lowest_set_bit(uintptr_t n) {
    return n & (~n + 1u);
}

static uintptr_t
block_base(void) {
    fixture();
    const uintptr_t base = (uintptr_t)app_arena_take(1, 1);
    fixture();
    return base;
}

static void
test_padding_alone_past_the_block_end_is_refused(void) {
    const uintptr_t end = block_base() + APP_ARENA_BYTES;
    const size_t align = (size_t)(2u * lowest_set_bit(end)); /* the next such boundary lies past the end */

    TEST_ASSERT_NOT_NULL(app_arena_take(APP_ARENA_BYTES - 1, 1));
    TEST_ASSERT_NULL(app_arena_take(1, align));
    TEST_ASSERT_EQUAL_UINT(APP_ARENA_BYTES - 1, app_arena_used());
}

static void
test_alignment_is_of_the_address_not_the_offset(void) {
    const uintptr_t base = block_base();
    const size_t align = (size_t)(2u * lowest_set_bit(base)); /* above the block's own */
    if (align >= APP_ARENA_BYTES) {
        TEST_IGNORE_MESSAGE("the block is already aligned beyond its own size");
    }

    TEST_ASSERT_NOT_NULL(app_arena_take(1, 1));
    const uintptr_t p = (uintptr_t)app_arena_take(1, align);
    TEST_ASSERT_NOT_EQUAL(0, p);
    TEST_ASSERT_EQUAL_UINT(0, p % align);
}

static void
test_zero_size_takes_nothing(void) {
    fixture();
    TEST_ASSERT_NULL(app_arena_take(0, 1));
    TEST_ASSERT_EQUAL_UINT(0, app_arena_used());
}

static void
test_an_alignment_that_is_not_a_power_of_two_is_refused(void) {
    fixture();
    TEST_ASSERT_NULL(app_arena_take(8, 0));
    TEST_ASSERT_NULL(app_arena_take(8, 3));
    TEST_ASSERT_NULL(app_arena_take(8, 24));
    TEST_ASSERT_EQUAL_UINT(0, app_arena_used());
}

static void
test_rewind_gives_back_everything_since_the_mark(void) {
    fixture();
    TEST_ASSERT_NOT_NULL(app_arena_take(10, 1));
    const size_t mark = app_arena_mark();
    void* first = app_arena_take(1000, 16);
    TEST_ASSERT_NOT_NULL(app_arena_take(5000, 16));

    app_arena_rewind(mark);
    TEST_ASSERT_EQUAL_UINT(10, app_arena_used());
    TEST_ASSERT_EQUAL_PTR(first, app_arena_take(1000, 16));
}

static void
test_nested_marks_rewind_inner_then_outer(void) {
    fixture();
    const size_t outer = app_arena_mark();
    TEST_ASSERT_NOT_NULL(app_arena_take(100, 1));
    const size_t inner = app_arena_mark();
    TEST_ASSERT_NOT_NULL(app_arena_take(100, 1));

    app_arena_rewind(inner);
    TEST_ASSERT_EQUAL_UINT(100, app_arena_used());
    app_arena_rewind(outer);
    TEST_ASSERT_EQUAL_UINT(0, app_arena_used());
}

static void
test_reset_empties_the_arena_and_hands_out_the_same_memory_again(void) {
    fixture();
    void* first = app_arena_take(64, 64);
    TEST_ASSERT_NOT_NULL(app_arena_take(APP_ARENA_BYTES / 2, 1));

    app_arena_reset();
    TEST_ASSERT_EQUAL_UINT(0, app_arena_used());
    TEST_ASSERT_EQUAL_PTR(first, app_arena_take(64, 64));
}

#ifdef HOST_HEAP_ARENA
static void
test_the_psram_heap_offers_only_what_the_arena_leaves(void) {
    if (getenv("HOST_HEAP_ARENA_PSRAM_BYTES") != NULL) {
        TEST_IGNORE_MESSAGE("HOST_HEAP_ARENA_PSRAM_BYTES overrides the modelled PSRAM");
    }
    const size_t heap_bytes = (size_t)HOST_HEAP_ARENA_PSRAM_BYTES - APP_ARENA_BYTES;

    TEST_ASSERT_LESS_OR_EQUAL_UINT(heap_bytes, heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    TEST_ASSERT_NULL(heap_caps_malloc(heap_bytes + 1, MALLOC_CAP_SPIRAM));
}
#endif

#ifdef DEVICE_BUILD
static void
test_the_psram_heap_starts_past_the_block(void) {
    TEST_ASSERT_LESS_OR_EQUAL_UINT(esp_psram_get_size() - APP_ARENA_BYTES, heap_caps_get_total_size(MALLOC_CAP_SPIRAM));
}

static void
test_a_taken_block_lies_in_external_ram(void) {
    fixture();
    TEST_ASSERT_TRUE(esp_ptr_external_ram(app_arena_take(1, 1)));
}
#endif

void
run_app_arena_suite(void) {
    RUN_TEST(test_every_power_of_two_alignment_is_honoured);
    RUN_TEST(test_consecutive_blocks_do_not_overlap);
    RUN_TEST(test_the_whole_block_can_be_taken_at_once);
    RUN_TEST(test_exhaustion_refuses_without_taking_and_the_arena_stays_usable);
    RUN_TEST(test_padding_alone_past_the_block_end_is_refused);
    RUN_TEST(test_alignment_is_of_the_address_not_the_offset);
    RUN_TEST(test_zero_size_takes_nothing);
    RUN_TEST(test_an_alignment_that_is_not_a_power_of_two_is_refused);
    RUN_TEST(test_rewind_gives_back_everything_since_the_mark);
    RUN_TEST(test_nested_marks_rewind_inner_then_outer);
    RUN_TEST(test_reset_empties_the_arena_and_hands_out_the_same_memory_again);
#ifdef HOST_HEAP_ARENA
    RUN_TEST(test_the_psram_heap_offers_only_what_the_arena_leaves);
#endif
#ifdef DEVICE_BUILD
    RUN_TEST(test_the_psram_heap_starts_past_the_block);
    RUN_TEST(test_a_taken_block_lies_in_external_ram);
#endif
}

SUITE_REGISTER(run_app_arena_suite);

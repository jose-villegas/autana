#include "suites.h"
#include "unity.h"

#include <stdint.h>
#include <string.h>

#include "app/app_arena.h"
#include "core/memory.h"
#include "testkit/cleanup.h"

static size_t suite_entry_mark;

static void
release_fixture(void) {
    app_arena_rewind(0);
}

static void
fixture(void) {
    TEST_ASSERT_EQUAL_size_t_MESSAGE(0, suite_entry_mark, "the app running the suites holds arena memory");
    app_arena_rewind(0);
    suite_set_test_cleanup(release_fixture);
}

static void
test_every_power_of_two_alignment_is_honoured(void) {
    fixture();
    for (size_t align = 1; align <= 4096; align *= 2) {
        TEST_ASSERT_NOT_NULL(app_arena_take(1, 1));
        const uintptr_t p = (uintptr_t)app_arena_take(3, align);
        TEST_ASSERT_NOT_EQUAL(0, p);
        TEST_ASSERT_EQUAL_size_t(0, p % align);
    }
}

static void
test_padding_is_counted_and_consecutive_takes_share_a_chunk(void) {
    fixture();
    const uintptr_t base = (uintptr_t)app_arena_take(1, 1);
    const uintptr_t aligned = (uintptr_t)app_arena_take(8, 64);
    TEST_ASSERT_NOT_EQUAL(0, base);
    TEST_ASSERT_NOT_EQUAL(0, aligned);
    TEST_ASSERT_EQUAL_size_t(aligned - base + 8, app_arena_mark());
    TEST_ASSERT_EQUAL_PTR((void*)(aligned + 8), app_arena_take(1, 1));
}

static void
test_crossing_a_chunk_end_opens_another_block(void) {
    fixture();
    const size_t before = app_heap_blocks();
    unsigned char* first = app_arena_take(16, 1);
    TEST_ASSERT_NOT_NULL(first);
    TEST_ASSERT_EQUAL_size_t(before + 1, app_heap_blocks());
    const size_t mark = app_arena_mark();
    const size_t free_at_mark = memory_free_bytes(MEMORY_APP);
    unsigned char* next = app_arena_take(APP_ARENA_CHUNK_BYTES, 1);
    TEST_ASSERT_NOT_NULL(next);
    TEST_ASSERT_TRUE(next != first + 16);
    TEST_ASSERT_EQUAL_size_t(before + 2, app_heap_blocks());
    app_arena_rewind(mark);
    TEST_ASSERT_EQUAL_PTR(first + 16, app_arena_take(1, 1));
    app_arena_rewind(mark);
    /* The emptied chunk stays for the next take that spills, as a frame's
     * scratch does every frame, and trimming gives it back. */
    TEST_ASSERT_EQUAL_PTR(next, app_arena_take(APP_ARENA_CHUNK_BYTES, 1));
    TEST_ASSERT_EQUAL_size_t(before + 2, app_heap_blocks());
    app_arena_rewind(mark);
    TEST_ASSERT_EQUAL_size_t(before + 2, app_heap_blocks());
    app_arena_trim();
    TEST_ASSERT_EQUAL_size_t(before + 1, app_heap_blocks());
    TEST_ASSERT_EQUAL_size_t(free_at_mark, memory_free_bytes(MEMORY_APP));
}

static void
test_a_large_take_is_one_contiguous_block(void) {
    fixture();
    const size_t before = app_heap_blocks();
    const size_t bytes = 2 * APP_ARENA_CHUNK_BYTES;
    unsigned char* block = app_arena_take(bytes, 64);
    TEST_ASSERT_NOT_NULL(block);
    memset(block, 0xA5, bytes);
    TEST_ASSERT_EQUAL_HEX8(0xA5, block[bytes - 1]);
    TEST_ASSERT_EQUAL_size_t(before + 1, app_heap_blocks());
}

static void
test_exhaustion_refuses_without_taking_and_the_arena_stays_usable(void) {
    fixture();
    TEST_ASSERT_NOT_NULL(app_arena_take(1, 1));
    const size_t mark = app_arena_mark();
    const size_t free_before = memory_free_bytes(MEMORY_APP);
    const size_t blocks_before = app_heap_blocks();
    TEST_ASSERT_NULL(app_arena_take(app_arena_bytes(), 1));
    TEST_ASSERT_NULL(app_arena_take(SIZE_MAX, 1));
    TEST_ASSERT_EQUAL_size_t(mark, app_arena_mark());
    TEST_ASSERT_EQUAL_size_t(free_before, memory_free_bytes(MEMORY_APP));
    TEST_ASSERT_EQUAL_size_t(blocks_before, app_heap_blocks());
    TEST_ASSERT_NOT_NULL(app_arena_take(64, 1));
}

static void
test_invalid_requests_take_nothing(void) {
    fixture();
    TEST_ASSERT_NULL(app_arena_take(0, 1));
    TEST_ASSERT_NULL(app_arena_take(8, 0));
    TEST_ASSERT_NULL(app_arena_take(8, 3));
    TEST_ASSERT_NULL(app_arena_take(8, 24));
    TEST_ASSERT_EQUAL_size_t(0, app_arena_mark());
}

static void
test_nested_marks_rewind_inner_then_outer(void) {
    fixture();
    TEST_ASSERT_NOT_NULL(app_arena_take(100, 1));
    const size_t outer = app_arena_mark();
    const size_t blocks_at_outer = app_heap_blocks();
    TEST_ASSERT_NOT_NULL(app_arena_take(APP_ARENA_CHUNK_BYTES, 16));
    const size_t inner = app_arena_mark();
    TEST_ASSERT_NOT_NULL(app_arena_take(APP_ARENA_CHUNK_BYTES, 16));
    app_arena_rewind(inner);
    TEST_ASSERT_EQUAL_size_t(inner, app_arena_mark());
    app_arena_rewind(outer);
    TEST_ASSERT_EQUAL_size_t(outer, app_arena_mark());
    app_arena_trim();
    TEST_ASSERT_EQUAL_size_t(blocks_at_outer, app_heap_blocks());
}

static void
test_the_heap_is_empty_on_the_next_visit(void) {
    fixture();
    const size_t empty = memory_free_bytes(MEMORY_APP);
    TEST_ASSERT_EQUAL_size_t(0, app_heap_blocks());
    TEST_ASSERT_NOT_NULL(memory_alloc(APP_ARENA_CHUNK_BYTES, MEMORY_APP));
    TEST_ASSERT_NOT_NULL(app_arena_take(APP_ARENA_CHUNK_BYTES, 64));
    app_arena_exit();
    app_arena_enter();
    TEST_ASSERT_EQUAL_size_t(0, app_heap_blocks());
    TEST_ASSERT_EQUAL_size_t(0, app_arena_mark());
    TEST_ASSERT_EQUAL_size_t(empty, memory_free_bytes(MEMORY_APP));
}

static void
test_the_region_is_the_psram_heap_less_the_system_reserve(void) {
    fixture();
    TEST_ASSERT_EQUAL_size_t(memory_total_bytes(MEMORY_PSRAM) - APP_ARENA_SYSTEM_RESERVE_BYTES, app_arena_bytes());
    TEST_ASSERT_TRUE(memory_in_psram(app_arena_take(1, 1)));
}

static void
test_a_framebuffer_sized_buffer_still_fits_beside_the_region(void) {
    fixture();
    void* buffer = memory_alloc(GFX_FRAMEBUFFER_BYTES, MEMORY_PSRAM);
    memory_free(buffer);
    TEST_ASSERT_NOT_NULL(buffer);
}

void
run_app_arena_suite(void) {
    suite_entry_mark = app_arena_mark();
    RUN_TEST(test_every_power_of_two_alignment_is_honoured);
    RUN_TEST(test_padding_is_counted_and_consecutive_takes_share_a_chunk);
    RUN_TEST(test_crossing_a_chunk_end_opens_another_block);
    RUN_TEST(test_a_large_take_is_one_contiguous_block);
    RUN_TEST(test_exhaustion_refuses_without_taking_and_the_arena_stays_usable);
    RUN_TEST(test_invalid_requests_take_nothing);
    RUN_TEST(test_nested_marks_rewind_inner_then_outer);
    RUN_TEST(test_the_heap_is_empty_on_the_next_visit);
    RUN_TEST(test_the_region_is_the_psram_heap_less_the_system_reserve);
    RUN_TEST(test_a_framebuffer_sized_buffer_still_fits_beside_the_region);
}

SUITE_REGISTER(run_app_arena_suite);

/*
 * Host-only: memory.h over heap_arena.c's two modeled pools. On the board the
 * same calls reach ESP-IDF's heap, whose budget a test cannot pin.
 */

#include "suites.h"

#ifndef DEVICE_BUILD

#include <string.h>

#include "unity.h"
#include "util/memory.h"

#define SMALL_BYTES 64

typedef struct {
    memory_kind_t kind;
    const char* name;
} memory_case_t;

static const memory_case_t EVERY_KIND[] = {
    {MEMORY_INTERNAL, "internal"},
    {MEMORY_8BIT, "8bit"},
    {MEMORY_DMA, "dma"},
    {MEMORY_PSRAM, "psram"},
};

/* Each kind with a pool of its own, and a kind on the other pool. */
static const struct {
    memory_kind_t kind;
    memory_kind_t other;
    const char* name;
} SEPARATE_POOLS[] = {
    {MEMORY_INTERNAL, MEMORY_PSRAM, "internal"},
    {MEMORY_DMA, MEMORY_PSRAM, "dma"},
    {MEMORY_PSRAM, MEMORY_INTERNAL, "psram"},
};

static void
test_a_small_block_of_every_kind_is_writable_and_comes_back(void) {
    for (size_t i = 0; i < sizeof EVERY_KIND / sizeof EVERY_KIND[0]; i++) {
        const memory_case_t* c = &EVERY_KIND[i];
        const size_t free_before = memory_free_bytes(c->kind);

        unsigned char* block = memory_alloc(SMALL_BYTES, c->kind);
        TEST_ASSERT_NOT_NULL_MESSAGE(block, c->name);
        memset(block, 0xA5, SMALL_BYTES);
        TEST_ASSERT_LESS_THAN_size_t_MESSAGE(free_before, memory_free_bytes(c->kind), c->name);

        memory_free(block);
        TEST_ASSERT_EQUAL_size_t_MESSAGE(free_before, memory_free_bytes(c->kind), c->name);
    }
}

static void
test_a_request_past_its_kinds_budget_fails_and_spares_the_other_pool(void) {
    for (size_t i = 0; i < sizeof SEPARATE_POOLS / sizeof SEPARATE_POOLS[0]; i++) {
        const size_t other_free = memory_free_bytes(SEPARATE_POOLS[i].other);

        void* block = memory_alloc(memory_free_bytes(SEPARATE_POOLS[i].kind) + 1, SEPARATE_POOLS[i].kind);

        TEST_ASSERT_NULL_MESSAGE(block, SEPARATE_POOLS[i].name);
        TEST_ASSERT_EQUAL_size_t_MESSAGE(other_free, memory_free_bytes(SEPARATE_POOLS[i].other),
                                         SEPARATE_POOLS[i].name);
    }
}

static void
test_a_block_of_one_kind_leaves_the_other_pools_budget_alone(void) {
    for (size_t i = 0; i < sizeof SEPARATE_POOLS / sizeof SEPARATE_POOLS[0]; i++) {
        const size_t other_free = memory_free_bytes(SEPARATE_POOLS[i].other);

        void* block = memory_alloc(16 * 1024, SEPARATE_POOLS[i].kind);
        TEST_ASSERT_NOT_NULL_MESSAGE(block, SEPARATE_POOLS[i].name);
        TEST_ASSERT_EQUAL_size_t_MESSAGE(other_free, memory_free_bytes(SEPARATE_POOLS[i].other),
                                         SEPARATE_POOLS[i].name);

        memory_free(block);
    }
}

static void
test_every_kind_reports_a_total_no_smaller_than_its_free_space_or_largest_block(void) {
    for (size_t i = 0; i < sizeof EVERY_KIND / sizeof EVERY_KIND[0]; i++) {
        const memory_case_t* c = &EVERY_KIND[i];
        const size_t free_bytes = memory_free_bytes(c->kind);

        TEST_ASSERT_GREATER_THAN_size_t_MESSAGE(0, free_bytes, c->name);
        TEST_ASSERT_LESS_OR_EQUAL_size_t_MESSAGE(free_bytes, memory_largest_block(c->kind), c->name);
        TEST_ASSERT_GREATER_OR_EQUAL_size_t_MESSAGE(free_bytes, memory_total_bytes(c->kind), c->name);
    }
}

static void
test_freeing_null_and_dumping_change_nothing(void) {
    for (size_t i = 0; i < sizeof EVERY_KIND / sizeof EVERY_KIND[0]; i++) {
        const memory_case_t* c = &EVERY_KIND[i];
        const size_t free_before = memory_free_bytes(c->kind);

        memory_free(NULL);
        memory_dump(c->kind);

        TEST_ASSERT_EQUAL_size_t_MESSAGE(free_before, memory_free_bytes(c->kind), c->name);
    }
}

void
suite_memory(void) {
    RUN_TEST(test_a_small_block_of_every_kind_is_writable_and_comes_back);
    RUN_TEST(test_a_request_past_its_kinds_budget_fails_and_spares_the_other_pool);
    RUN_TEST(test_a_block_of_one_kind_leaves_the_other_pools_budget_alone);
    RUN_TEST(test_every_kind_reports_a_total_no_smaller_than_its_free_space_or_largest_block);
    RUN_TEST(test_freeing_null_and_dumping_change_nothing);
}

#else

void
suite_memory(void) {}

#endif

SUITE_REGISTER(suite_memory)

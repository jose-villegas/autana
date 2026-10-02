/*
 * memory.h's kinds. On a host, over heap_arena.c's two modeled pools: their
 * budgets are known, so a request past one can be shown to fail. On the board,
 * where the budget cannot be pinned, that each kind's block lands in the
 * memory the kind names.
 */

#include "suites.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"

#include "util/memory.h"

#ifdef DEVICE_BUILD
#include "esp_memory_utils.h"

static void
test_each_kinds_block_lands_in_the_memory_it_names(void) {
    static const struct {
        memory_kind_t kind;
        bool external;
        bool dma;
        const char* name;
    } PLACEMENT[] = {
        {MEMORY_INTERNAL, false, false, "internal"},
        {MEMORY_DMA, false, true, "dma"},
        {MEMORY_PSRAM, true, false, "psram"},
    };

    for (size_t i = 0; i < sizeof PLACEMENT / sizeof PLACEMENT[0]; i++) {
        unsigned char* block = memory_alloc(64, PLACEMENT[i].kind);
        TEST_ASSERT_NOT_NULL_MESSAGE(block, PLACEMENT[i].name);
        block[63] = 0xA5;
        TEST_ASSERT_EQUAL_MESSAGE(PLACEMENT[i].external, esp_ptr_external_ram(block), PLACEMENT[i].name);
        TEST_ASSERT_EQUAL_MESSAGE(!PLACEMENT[i].external, esp_ptr_internal(block), PLACEMENT[i].name);
        if (PLACEMENT[i].dma) {
            TEST_ASSERT_TRUE_MESSAGE(esp_ptr_dma_capable(block), PLACEMENT[i].name);
        }
        memory_free(block);
    }
}

void
suite_memory(void) {
    RUN_TEST(test_each_kinds_block_lands_in_the_memory_it_names);
}

#else

#include "heap_arena.h"

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

/* A total is capacity, not free space: holding a block moves one, not the
 * other. */
static void
test_a_kinds_total_is_its_pools_capacity_while_a_block_is_held(void) {
    const size_t internal = heap_arena_internal_heap_bytes(getenv("HOST_HEAP_ARENA_BYTES"));
    const size_t psram = heap_arena_psram_heap_bytes(getenv("HOST_HEAP_ARENA_PSRAM_BYTES"));

    const struct {
        memory_kind_t kind;
        size_t capacity;
        const char* name;
    } TOTALS[] = {
        {MEMORY_INTERNAL, internal, "internal"},
        {MEMORY_DMA, internal, "dma"},
        {MEMORY_PSRAM, psram, "psram"},
        {MEMORY_8BIT, internal + psram, "8bit"},
    };

    for (size_t i = 0; i < sizeof TOTALS / sizeof TOTALS[0]; i++) {
        const size_t free_before = memory_free_bytes(TOTALS[i].kind);
        TEST_ASSERT_EQUAL_size_t_MESSAGE(TOTALS[i].capacity, memory_total_bytes(TOTALS[i].kind), TOTALS[i].name);

        void* block = memory_alloc(4096, TOTALS[i].kind);
        TEST_ASSERT_NOT_NULL_MESSAGE(block, TOTALS[i].name);
        TEST_ASSERT_LESS_THAN_size_t_MESSAGE(free_before, memory_free_bytes(TOTALS[i].kind), TOTALS[i].name);
        TEST_ASSERT_EQUAL_size_t_MESSAGE(TOTALS[i].capacity, memory_total_bytes(TOTALS[i].kind), TOTALS[i].name);

        memory_free(block);
    }
}

void
suite_memory(void) {
    RUN_TEST(test_a_small_block_of_every_kind_is_writable_and_comes_back);
    RUN_TEST(test_a_request_past_its_kinds_budget_fails_and_spares_the_other_pool);
    RUN_TEST(test_a_block_of_one_kind_leaves_the_other_pools_budget_alone);
    RUN_TEST(test_every_kind_reports_a_total_no_smaller_than_its_free_space_or_largest_block);
    RUN_TEST(test_a_kinds_total_is_its_pools_capacity_while_a_block_is_held);
    RUN_TEST(test_freeing_null_and_dumping_change_nothing);
}

#endif

SUITE_REGISTER(suite_memory)

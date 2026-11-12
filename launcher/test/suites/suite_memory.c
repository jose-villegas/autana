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

#include "core/memory.h"

/* A restart forgets a low already passed and follows the next one down. Loose
 * enough for the board, where another task may allocate in between: a block
 * half the largest one moves every reading far past that. */
static void
test_a_restarted_low_water_mark_forgets_the_low_before_it(void) {
    const size_t bytes = memory_largest_block(MEMORY_PSRAM) / 2;
    memory_free(memory_alloc(bytes, MEMORY_PSRAM));
    const size_t low_passed = memory_min_free_bytes(MEMORY_PSRAM);

    memory_min_free_restart();
    const size_t low_restarted = memory_min_free_bytes(MEMORY_PSRAM);
    void* block = memory_alloc(bytes, MEMORY_PSRAM);
    const size_t free_held = memory_free_bytes(MEMORY_PSRAM);
    const size_t low_held = memory_min_free_bytes(MEMORY_PSRAM);
    memory_free(block);

    TEST_ASSERT_NOT_NULL(block);
    TEST_ASSERT_GREATER_THAN_size_t(low_passed + bytes / 2, low_restarted);
    TEST_ASSERT_LESS_THAN_size_t(low_restarted - bytes / 2, low_held);
    TEST_ASSERT_LESS_OR_EQUAL_size_t(free_held, low_held);
}

#define TEST_BLOCK_BYTES    4096u
/* A hole between two live blocks, sized off any alignment step so that
 * memalign's padding search on a host is exercised: room for a pointer and
 * a few alignment units. */
#define APP_TEST_HOLE_BYTES (3u * _Alignof(max_align_t) + sizeof(void*))

static void
test_app_largest_block_is_allocatable_until_exhaustion(void) {
    void* head = NULL;
    void* before = memory_alloc(APP_TEST_HOLE_BYTES, MEMORY_APP);
    void* hole = memory_alloc(APP_TEST_HOLE_BYTES, MEMORY_APP);
    void* guard = memory_alloc(APP_TEST_HOLE_BYTES, MEMORY_APP);
    bool allocatable = before != NULL && hole != NULL && guard != NULL;
    size_t largest;
    while ((largest = memory_largest_block(MEMORY_APP)) >= sizeof(void*)) {
        void* block = memory_alloc(largest, MEMORY_APP);
        if (block == NULL) {
            allocatable = false;
            break;
        }
        *(void**)block = head;
        head = block;
    }
    memory_free(hole);
    largest = memory_largest_block(MEMORY_APP);
    void* refill = memory_alloc(largest, MEMORY_APP);
    if (largest != 0 && refill == NULL) {
        allocatable = false;
    }
    memory_free(refill);
    memory_free(guard);
    memory_free(before);
    while (head != NULL) {
        void* next = *(void**)head;
        memory_free(head);
        head = next;
    }
    TEST_ASSERT_TRUE(allocatable);
}

static void
test_app_allocations_and_queries_use_the_region(void) {
    const size_t free_before = memory_free_bytes(MEMORY_APP);
    const size_t total = memory_total_bytes(MEMORY_APP);
    const size_t system_free = memory_free_bytes(MEMORY_PSRAM);
    const size_t blocks = app_heap_blocks();
    memory_min_free_restart();
    unsigned char* block = memory_alloc(TEST_BLOCK_BYTES, MEMORY_APP);
    TEST_ASSERT_NOT_NULL(block);
    TEST_ASSERT_TRUE(app_heap_contains(block));
    TEST_ASSERT_TRUE(memory_in_psram(block));
    TEST_ASSERT_GREATER_OR_EQUAL_size_t(TEST_BLOCK_BYTES, memory_block_bytes(block));
    memset(block, 0xA5, TEST_BLOCK_BYTES);
    TEST_ASSERT_LESS_THAN_size_t(free_before, memory_free_bytes(MEMORY_APP));
    TEST_ASSERT_LESS_OR_EQUAL_size_t(memory_free_bytes(MEMORY_APP), memory_min_free_bytes(MEMORY_APP));
    TEST_ASSERT_GREATER_THAN_size_t(0, memory_largest_block(MEMORY_APP));
    TEST_ASSERT_EQUAL_size_t(total, memory_total_bytes(MEMORY_APP));
    /* Other tasks move the system heap by a few bytes; the block is not
     * among them. */
    const size_t system_now = memory_free_bytes(MEMORY_PSRAM);
    TEST_ASSERT_LESS_THAN_size_t(TEST_BLOCK_BYTES, system_free > system_now ? system_free - system_now : 0);
    memory_free(block);
    TEST_ASSERT_EQUAL_size_t(blocks, app_heap_blocks());
    TEST_ASSERT_EQUAL_size_t(free_before, memory_free_bytes(MEMORY_APP));
    TEST_ASSERT_LESS_THAN_size_t(free_before, memory_min_free_bytes(MEMORY_APP));
    memory_min_free_restart();
    TEST_ASSERT_EQUAL_size_t(free_before, memory_min_free_bytes(MEMORY_APP));
}

/* Rounding down to a size class stays within one class of the request and
 * is already on one. */
static void
test_an_unrounded_request_is_one_class_below_at_most(void) {
    for (size_t bytes = TEST_BLOCK_BYTES; bytes < SIZE_MAX / 3; bytes = bytes * 3 + 1) {
        const size_t unrounded = memory_unrounded_bytes(bytes);
        TEST_ASSERT_LESS_OR_EQUAL_size_t(bytes, unrounded);
        TEST_ASSERT_LESS_THAN_size_t(bytes / 16, bytes - unrounded);
        TEST_ASSERT_EQUAL_size_t(unrounded, memory_unrounded_bytes(unrounded));
    }
}

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
        TEST_ASSERT_EQUAL_MESSAGE(PLACEMENT[i].external, memory_in_psram(block), PLACEMENT[i].name);
        TEST_ASSERT_EQUAL_MESSAGE(!PLACEMENT[i].external, esp_ptr_internal(block), PLACEMENT[i].name);
        if (PLACEMENT[i].dma) {
            TEST_ASSERT_TRUE_MESSAGE(esp_ptr_dma_capable(block), PLACEMENT[i].name);
        }
        memory_free(block);
    }
}

/* The heap gives an unrounded request exactly what it asks, so a region
 * sized that way leaves the system its whole reserve. */
static void
test_the_psram_heap_gives_an_unrounded_request_exactly(void) {
    const size_t bytes = memory_unrounded_bytes(memory_largest_block(MEMORY_PSRAM) / 2);
    void* block = memory_alloc(bytes, MEMORY_PSRAM);
    TEST_ASSERT_NOT_NULL(block);
    const size_t given = memory_block_bytes(block);
    memory_free(block);
    TEST_ASSERT_EQUAL_size_t(bytes, given);
}

/* A fast block small enough to fit beside the reserve in the self-test
 * image, which has a few KB on chip past it. */
#define FAST_BESIDE_RESERVE_BYTES 1024u

/* A fast block lands on chip while the reserve stays free after it, and
 * goes to external RAM once it would cut into the reserve. */
static void
test_a_fast_block_leaves_the_reserve_free_on_chip(void) {
    const size_t free_on_chip = memory_free_bytes(MEMORY_INTERNAL);
    TEST_ASSERT_GREATER_THAN_size_t(MEMORY_FAST_INTERNAL_RESERVE + FAST_BESIDE_RESERVE_BYTES, free_on_chip);

    void* kept = memory_alloc(FAST_BESIDE_RESERVE_BYTES, MEMORY_FAST);
    const bool kept_on_chip = kept != NULL && !memory_in_psram(kept);
    memory_free(kept);
    void* spilled = memory_alloc(free_on_chip - MEMORY_FAST_INTERNAL_RESERVE + 1, MEMORY_FAST);
    const bool spilled_external = spilled != NULL && memory_in_psram(spilled);
    memory_free(spilled);

    TEST_ASSERT_TRUE_MESSAGE(kept_on_chip, "a block that leaves the reserve stays on chip");
    TEST_ASSERT_TRUE_MESSAGE(spilled_external, "a block that cuts into the reserve goes to external RAM");
}

void
suite_memory(void) {
    RUN_TEST(test_app_allocations_and_queries_use_the_region);
    RUN_TEST(test_app_largest_block_is_allocatable_until_exhaustion);
    RUN_TEST(test_an_unrounded_request_is_one_class_below_at_most);
    RUN_TEST(test_the_psram_heap_gives_an_unrounded_request_exactly);
    RUN_TEST(test_each_kinds_block_lands_in_the_memory_it_names);
    RUN_TEST(test_a_restarted_low_water_mark_forgets_the_low_before_it);
    RUN_TEST(test_a_fast_block_leaves_the_reserve_free_on_chip);
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
test_only_a_psram_block_reads_as_in_psram(void) {
    for (size_t i = 0; i < sizeof SEPARATE_POOLS / sizeof SEPARATE_POOLS[0]; i++) {
        void* block = memory_alloc(SMALL_BYTES, SEPARATE_POOLS[i].kind);
        TEST_ASSERT_NOT_NULL_MESSAGE(block, SEPARATE_POOLS[i].name);
        TEST_ASSERT_EQUAL_MESSAGE(SEPARATE_POOLS[i].kind == MEMORY_PSRAM, memory_in_psram(block),
                                  SEPARATE_POOLS[i].name);
        memory_free(block);
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

/* A fast block stays on-chip while it fits there, uncounted. */
static void
test_a_fast_block_that_fits_on_chip_lands_there(void) {
    const memory_fallbacks_t before = memory_fast_fallbacks();
    void* block = memory_alloc(SMALL_BYTES, MEMORY_FAST);
    TEST_ASSERT_NOT_NULL(block);
    TEST_ASSERT_FALSE(memory_in_psram(block));
    memory_free(block);
    TEST_ASSERT_EQUAL_UINT32(before.count, memory_fast_fallbacks().count);
}

/* One past what on-chip RAM can give lands in external RAM instead of
 * failing, and is counted with its bytes. */
static void
test_a_fast_block_past_on_chip_ram_falls_back_and_is_counted(void) {
    const memory_fallbacks_t before = memory_fast_fallbacks();
    const size_t bytes = memory_free_bytes(MEMORY_INTERNAL) + 1;
    void* block = memory_alloc(bytes, MEMORY_FAST);
    TEST_ASSERT_NOT_NULL(block);
    TEST_ASSERT_TRUE(memory_in_psram(block));
    const memory_fallbacks_t after = memory_fast_fallbacks();
    memory_dump(MEMORY_FAST);
    memory_free(block);
    TEST_ASSERT_EQUAL_UINT32(before.count + 1, after.count);
    TEST_ASSERT_EQUAL_size_t(before.bytes + bytes, after.bytes);
}

/* Fast fails only when neither RAM can give the block. */
static void
test_a_fast_block_past_both_rams_fails_uncounted(void) {
    const memory_fallbacks_t before = memory_fast_fallbacks();
    TEST_ASSERT_NULL(memory_alloc(memory_free_bytes(MEMORY_FAST) + 1, MEMORY_FAST));
    TEST_ASSERT_EQUAL_UINT32(before.count, memory_fast_fallbacks().count);
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

        void* block = memory_alloc(TEST_BLOCK_BYTES, TOTALS[i].kind);
        TEST_ASSERT_NOT_NULL_MESSAGE(block, TOTALS[i].name);
        TEST_ASSERT_LESS_THAN_size_t_MESSAGE(free_before, memory_free_bytes(TOTALS[i].kind), TOTALS[i].name);
        TEST_ASSERT_EQUAL_size_t_MESSAGE(TOTALS[i].capacity, memory_total_bytes(TOTALS[i].kind), TOTALS[i].name);

        memory_free(block);
    }
}

/* A hole between two held blocks is free space no single block can reach:
 * the largest block answers for one allocation, not for the sum. */
static void
test_the_largest_block_is_less_than_free_space_once_a_pool_has_a_hole(void) {
    for (size_t i = 0; i < sizeof SEPARATE_POOLS / sizeof SEPARATE_POOLS[0]; i++) {
        const memory_kind_t kind = SEPARATE_POOLS[i].kind;
        void* before = memory_alloc(TEST_BLOCK_BYTES, kind);
        void* hole = memory_alloc(TEST_BLOCK_BYTES, kind);
        void* after = memory_alloc(TEST_BLOCK_BYTES, kind);
        TEST_ASSERT_NOT_NULL_MESSAGE(after, SEPARATE_POOLS[i].name);

        memory_free(hole);
        TEST_ASSERT_LESS_OR_EQUAL_size_t_MESSAGE(memory_free_bytes(kind) - TEST_BLOCK_BYTES, memory_largest_block(kind),
                                                 SEPARATE_POOLS[i].name);

        memory_free(before);
        memory_free(after);
    }
}

/* The low-water mark keeps a block freed again, never rises, and never
 * reads above the free space now. The harness restarts it each test, so one
 * near-largest block per pool sets an exact new low. */
static void
test_the_lowest_free_space_remembers_a_block_freed_again(void) {
    static const memory_case_t ONE_KIND_PER_POOL[] = {{MEMORY_INTERNAL, "internal"}, {MEMORY_PSRAM, "psram"}};
    for (size_t i = 0; i < sizeof ONE_KIND_PER_POOL / sizeof ONE_KIND_PER_POOL[0]; i++) {
        const memory_case_t* c = &ONE_KIND_PER_POOL[i];
        const size_t free_before = memory_free_bytes(c->kind);
        const size_t low_before = memory_min_free_bytes(c->kind);
        const size_t largest = memory_largest_block(c->kind);

        void* block = memory_alloc(largest - largest % SMALL_BYTES - SMALL_BYTES, c->kind);
        const size_t free_held = memory_free_bytes(c->kind);
        const size_t low_held = memory_min_free_bytes(c->kind);
        memory_free(block);

        TEST_ASSERT_NOT_NULL_MESSAGE(block, c->name);
        TEST_ASSERT_LESS_OR_EQUAL_size_t_MESSAGE(free_before, low_before, c->name);
        TEST_ASSERT_LESS_THAN_size_t_MESSAGE(low_before, free_held, c->name);
        TEST_ASSERT_EQUAL_size_t_MESSAGE(free_held, low_held, c->name);
        TEST_ASSERT_EQUAL_size_t_MESSAGE(free_before, memory_free_bytes(c->kind), c->name);
        TEST_ASSERT_EQUAL_size_t_MESSAGE(free_held, memory_min_free_bytes(c->kind), c->name);
    }
    for (size_t i = 0; i < sizeof EVERY_KIND / sizeof EVERY_KIND[0]; i++) {
        const memory_case_t* c = &EVERY_KIND[i];
        TEST_ASSERT_LESS_OR_EQUAL_size_t_MESSAGE(memory_free_bytes(c->kind), memory_min_free_bytes(c->kind), c->name);
    }
    TEST_ASSERT_EQUAL_size_t(memory_min_free_bytes(MEMORY_INTERNAL) + memory_min_free_bytes(MEMORY_PSRAM),
                             memory_min_free_bytes(MEMORY_8BIT));
}

void
suite_memory(void) {
    RUN_TEST(test_app_allocations_and_queries_use_the_region);
    RUN_TEST(test_app_largest_block_is_allocatable_until_exhaustion);
    RUN_TEST(test_an_unrounded_request_is_one_class_below_at_most);
    RUN_TEST(test_a_small_block_of_every_kind_is_writable_and_comes_back);
    RUN_TEST(test_the_lowest_free_space_remembers_a_block_freed_again);
    RUN_TEST(test_a_restarted_low_water_mark_forgets_the_low_before_it);
    RUN_TEST(test_the_largest_block_is_less_than_free_space_once_a_pool_has_a_hole);
    RUN_TEST(test_only_a_psram_block_reads_as_in_psram);
    RUN_TEST(test_a_request_past_its_kinds_budget_fails_and_spares_the_other_pool);
    RUN_TEST(test_a_block_of_one_kind_leaves_the_other_pools_budget_alone);
    RUN_TEST(test_a_fast_block_that_fits_on_chip_lands_there);
    RUN_TEST(test_a_fast_block_past_on_chip_ram_falls_back_and_is_counted);
    RUN_TEST(test_a_fast_block_past_both_rams_fails_uncounted);
    RUN_TEST(test_every_kind_reports_a_total_no_smaller_than_its_free_space_or_largest_block);
    RUN_TEST(test_a_kinds_total_is_its_pools_capacity_while_a_block_is_held);
    RUN_TEST(test_freeing_null_and_dumping_change_nothing);
}

#endif

SUITE_REGISTER(suite_memory)

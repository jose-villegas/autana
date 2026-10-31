#include <string.h>

#include "hash/crc32.h"
#include "testkit/bench.h"
#include "testkit/cleanup.h"
#include "testkit/suite.h"
#include "unity.h"

#define HASH_BENCH_CAPACITY      (64 * 1024)
#define HASH_BENCH_TYPE_CAPACITY 32

static const uint8_t flash_data[HASH_BENCH_CAPACITY] = {0};
static uint8_t* active_buffer;

typedef uint32_t (*crc32_fn_t)(uint32_t, const void*, size_t);

typedef struct {
    const char* name;
    crc32_fn_t update;
} candidate_t;

typedef struct {
    crc32_fn_t update;
    const uint8_t* data;
    size_t size;
    uint32_t result;
} cycle_context_t;

static void
release(void) {
    free(active_buffer);
    active_buffer = NULL;
}

static void
run_cycles(void* context) {
    cycle_context_t* c = context;
    c->result = c->update(0, c->data, c->size);
    __asm__ volatile("" : : "r"(c->result) : "memory");
}

static void
test_hash_board_rows(void) {
    if (bench_is_qemu()) {
        TEST_IGNORE_MESSAGE("QEMU has no hardware cache timing or performance monitor");
    }
    static const candidate_t candidates[] = {
        {"crc32_update_nibble", crc32_update_nibble},
#ifdef ESP_PLATFORM
        {"crc32_update_rom", crc32_update_rom},
#endif
    };
    static const size_t sizes[] = {64, 4 * 1024, HASH_BENCH_CAPACITY};
    static const char* const memories[] = {"ram", "flash"};
    active_buffer = bench_alloc_internal(HASH_BENCH_CAPACITY, "CRC input");
    suite_set_test_cleanup(release);
    memcpy(active_buffer, flash_data, HASH_BENCH_CAPACITY);
    bench_target_report("HASHTARGET");
    for (size_t candidate = 0; candidate < sizeof candidates / sizeof *candidates; candidate++) {
        for (size_t size = 0; size < sizeof sizes / sizeof *sizes; size++) {
            for (size_t memory = 0; memory < sizeof memories / sizeof *memories; memory++) {
                cycle_context_t context = {.update = candidates[candidate].update,
                                           .data = memory == 0 ? active_buffer : flash_data,
                                           .size = sizes[size]};
                char type[HASH_BENCH_TYPE_CAPACITY];
                snprintf(type, sizeof type, "%zu_%s", sizes[size], memories[memory]);
                bench_report_cycles("HASHBENCH", candidates[candidate].name, type, 1, run_cycles, &context);
                TEST_ASSERT_EQUAL_HEX32(crc32_update_nibble(0, context.data, context.size), context.result);
            }
        }
    }
    release();
    suite_clear_test_cleanup();
}

void
run_hash_perf_suite(void) {
    RUN_TEST(test_hash_board_rows);
}

SUITE_REGISTER_ON_REQUEST(run_hash_perf_suite);

/* bench: portable timing, allocation and measurement identity for benchmark suites. */
#pragma once

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "unity.h"

#define BENCH_CYCLE_WARM_REPS              31
#define BENCH_CYCLE_COLD_REPS              9

#define BENCH_ICOUNT_PREFIX                "ICOUNT"
#define BENCH_ICOUNT_INFO_PREFIX           "ICOUNT_INFO"
#define BENCH_ICOUNT_TOLERANCE_DENOMINATOR 200

/* A row needs at least MARGIN times the tolerance denominator us per timed
 * batch, keeping sub-us clock rounding below one MARGIN-th of the tolerance. */
#define BENCH_ICOUNT_ROUNDING_MARGIN       10

static inline bool
bench_icount_long_enough(int64_t us, int batches) {
    return us >= batches * BENCH_ICOUNT_ROUNDING_MARGIN * BENCH_ICOUNT_TOLERANCE_DENOMINATOR;
}

static inline void
bench_icount_report_row(const char* prefix, const char* name, int64_t us, int reps, int batches) {
    printf("%s %s us=%" PRId64 " reps=%d\n", prefix, name, us, reps);
    TEST_ASSERT_TRUE(bench_icount_long_enough(us, batches));
}

static inline void
bench_icount_report(const char* name, int64_t us, int reps, int batches) {
    bench_icount_report_row(BENCH_ICOUNT_PREFIX, name, us, reps, batches);
}

typedef struct {
    int64_t min, max, avg, med, p95;
} bench_stats_t;

bench_stats_t bench_stats_compute(int32_t* values, int n);

#ifdef ESP_PLATFORM
#include "esp_cpu.h"

static inline uint32_t
bench_cycles(void) {
    return esp_cpu_get_cycle_count();
}
#else
uint32_t bench_cycles(void);
#endif
bool bench_cache_cold(void);
bool bench_is_qemu(void);
void bench_cache_code_cold(void);
bool bench_insn_begin(void);
bool bench_insn_end(uint32_t* insn);
void* bench_alloc_internal(size_t size, const char* what);
void bench_yield(void);
int64_t bench_now_us(void);
const char* bench_build_id(void);
bool bench_counters_idle(void);
void bench_target_report(const char* prefix);

static inline float
bench_cycle_median(void (*run)(void* ctx), void* ctx, bool cold) {
    _Static_assert(BENCH_CYCLE_COLD_REPS <= BENCH_CYCLE_WARM_REPS, "cold samples must fit warm sample storage");
    int32_t samples[BENCH_CYCLE_WARM_REPS];
    const int reps = cold ? BENCH_CYCLE_COLD_REPS : BENCH_CYCLE_WARM_REPS;
    run(ctx);
    for (int rep = 0; rep < reps; rep++) {
        if (cold) {
            bench_cache_cold();
        }
        const uint32_t start = bench_cycles();
        run(ctx);
        samples[rep] = (int32_t)(bench_cycles() - start);
    }
    return (float)bench_stats_compute(samples, reps).med;
}

static inline void
bench_report_cycles(const char* prefix, const char* name, const char* type, int ops, void (*run)(void* ctx),
                    void* ctx) {
    const float warm = bench_cycle_median(run, ctx, false) / (float)ops;
    const bool cold = bench_cache_cold();
    printf("%s name=%s type=%s ops=%d warm=%.1f", prefix, name, type, ops, (double)warm);
    if (cold) {
        printf(" cold=%.1f", (double)bench_cycle_median(run, ctx, true));
    }
    run(ctx);
    const bool counting = bench_insn_begin();
    run(ctx);
    if (counting) {
        uint32_t value;
        TEST_ASSERT_TRUE(bench_insn_end(&value));
        printf(" insn=%.1f", (double)((float)value / (float)ops));
    }
    printf("\n");
    bench_yield();
}

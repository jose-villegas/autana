/* In-place timing statistics shared by device performance suites. */
#pragma once

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "unity.h"

#define PERF_CYCLE_WARM_REPS              31
#define PERF_CYCLE_COLD_REPS              9

#define PERF_ICOUNT_PREFIX                "ICOUNT"
#define PERF_ICOUNT_INFO_PREFIX           "ICOUNT_INFO"
#define PERF_ICOUNT_TOLERANCE_DENOMINATOR 200

/* A row needs at least MARGIN times the tolerance denominator us per timed
 * batch, keeping sub-us clock rounding below one MARGIN-th of the tolerance. */
#define PERF_ICOUNT_ROUNDING_MARGIN       10

static inline bool
perf_icount_long_enough(int64_t us, int batches) {
    return us >= batches * PERF_ICOUNT_ROUNDING_MARGIN * PERF_ICOUNT_TOLERANCE_DENOMINATOR;
}

static inline void
perf_icount_report_row(const char* prefix, const char* name, int64_t us, int reps, int batches) {
    printf("%s %s us=%" PRId64 " reps=%d\n", prefix, name, us, reps);
    TEST_ASSERT_TRUE(perf_icount_long_enough(us, batches));
}

static inline void
perf_icount_report(const char* name, int64_t us, int reps, int batches) {
    perf_icount_report_row(PERF_ICOUNT_PREFIX, name, us, reps, batches);
}

typedef struct {
    int64_t min, max, avg, med, p95;
} perf_stats_t;

static inline int
perf_stats_compare(const void* a, const void* b) {
    const int32_t va = *(const int32_t*)a;
    const int32_t vb = *(const int32_t*)b;
    return (va > vb) - (va < vb);
}

static inline perf_stats_t
perf_stats_compute(int32_t* values, int n) {
    perf_stats_t s = {.min = INT64_MAX, .max = 0, .avg = 0, .med = 0, .p95 = 0};
    int64_t sum = 0;
    for (int i = 0; i < n; i++) {
        if (values[i] < s.min) {
            s.min = values[i];
        }
        if (values[i] > s.max) {
            s.max = values[i];
        }
        sum += values[i];
    }
    s.avg = sum / n;
    qsort(values, n, sizeof(int32_t), perf_stats_compare);
    s.med = values[n / 2];
    s.p95 = values[(n * 95) / 100];
    return s;
}

#ifdef DEVICE_BUILD
#include "esp_cache.h"
#include "esp_cpu.h"
extern char _rodata_reserved_start[];
extern char _rodata_reserved_end[];
extern char _instruction_reserved_start[];
extern char _instruction_reserved_end[];

/* Cold calls refill instructions and flash rodata; PSRAM stays cached and inputs stay in internal RAM.
 * Instructions and flash rodata are read-only, so dropping them loses nothing. Writing back then
 * invalidating PSRAM would drop whatever the other core writes between those two steps. */
static inline void
perf_cache_rodata_cold(void) {
    const uintptr_t data_line = CONFIG_ESP32S3_DATA_CACHE_LINE_SIZE;
    const uintptr_t data_start = (uintptr_t)_rodata_reserved_start & ~(data_line - 1);
    const uintptr_t data_end = ((uintptr_t)_rodata_reserved_end + data_line - 1) & ~(data_line - 1);
    TEST_ASSERT_EQUAL(ESP_OK, esp_cache_msync((void*)data_start, data_end - data_start,
                                              ESP_CACHE_MSYNC_FLAG_TYPE_DATA | ESP_CACHE_MSYNC_FLAG_DIR_M2C));
}

static inline void
perf_cache_code_cold(void) {
    const uintptr_t line = CONFIG_ESP32S3_INSTRUCTION_CACHE_LINE_SIZE;
    const uintptr_t start = (uintptr_t)_instruction_reserved_start & ~(line - 1);
    const uintptr_t end = ((uintptr_t)_instruction_reserved_end + line - 1) & ~(line - 1);
    TEST_ASSERT_EQUAL(ESP_OK, esp_cache_msync((void*)start, end - start,
                                              ESP_CACHE_MSYNC_FLAG_TYPE_INST | ESP_CACHE_MSYNC_FLAG_DIR_M2C));
}

static inline void
perf_cache_cold(void) {
    perf_cache_rodata_cold();
    perf_cache_code_cold();
}

static inline float
perf_cycle_median(void (*run)(void* ctx), void* ctx, bool cold) {
    _Static_assert(PERF_CYCLE_COLD_REPS <= PERF_CYCLE_WARM_REPS, "cold samples must fit warm sample storage");
    int32_t samples[PERF_CYCLE_WARM_REPS];
    const int reps = cold ? PERF_CYCLE_COLD_REPS : PERF_CYCLE_WARM_REPS;
    run(ctx);
    for (int rep = 0; rep < reps; rep++) {
        if (cold) {
            perf_cache_cold();
        }
        const uint32_t start = esp_cpu_get_cycle_count();
        run(ctx);
        samples[rep] = (int32_t)(esp_cpu_get_cycle_count() - start);
    }
    return (float)perf_stats_compute(samples, reps).med;
}

#endif

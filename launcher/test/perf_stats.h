/* In-place timing statistics shared by device performance suites. */
#pragma once

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "unity.h"

/* Each batch's clock reads round by under 1 us; this length keeps rounding
 * below one MARGIN-th of the tolerance. */
#define PERF_ICOUNT_PREFIX                "ICOUNT"
#define PERF_ICOUNT_TOLERANCE_DENOMINATOR 200
#define PERF_ICOUNT_ROUNDING_MARGIN       10

static inline void
perf_icount_report(const char* name, int64_t us, int reps, int batches) {
    printf(PERF_ICOUNT_PREFIX " %s us=%" PRId64 " reps=%d\n", name, us, reps);
    TEST_ASSERT_TRUE(us >= batches * PERF_ICOUNT_ROUNDING_MARGIN * PERF_ICOUNT_TOLERANCE_DENOMINATOR);
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

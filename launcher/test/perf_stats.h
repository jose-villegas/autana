/* In-place timing statistics shared by device performance suites. */
#pragma once

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "math/stats/statsi.h"
#include "unity.h"

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
    s.med = statsi_median(values, (size_t)n);
    s.p95 = statsi_percentile(values, (size_t)n, 95);
    return s;
}

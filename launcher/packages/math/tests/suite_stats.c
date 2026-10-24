#include "math/stats/statsf.h"
#include "math/stats/statsi.h"
#include "suites.h"
#include "unity.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum { STATS_MAX = 101 };

static const unsigned percents[] = {0, 1, 50, 95, 99, 100};

static int
ascending_i32(const void* a, const void* b) {
    const int32_t x = *(const int32_t*)a;
    const int32_t y = *(const int32_t*)b;
    return (x > y) - (x < y);
}

static size_t
rank_of(size_t count, unsigned percent) {
    const size_t want = count * percent / 100U;
    return want < count ? want : count - 1;
}

/* Every percentile of every prefix count equals what a full sort puts at that rank. */
static void
expect_ranks_like_a_sort(const int32_t* source, size_t count) {
    int32_t* sorted = malloc(sizeof(int32_t) * count);
    int32_t* work = malloc(sizeof(int32_t) * count);
    TEST_ASSERT_NOT_NULL(sorted);
    TEST_ASSERT_NOT_NULL(work);
    memcpy(sorted, source, sizeof(int32_t) * count);
    qsort(sorted, count, sizeof sorted[0], ascending_i32);
    for (size_t p = 0; p < sizeof percents / sizeof percents[0]; p++) {
        memcpy(work, source, sizeof(int32_t) * count);
        TEST_ASSERT_EQUAL_INT32(sorted[rank_of(count, percents[p])], statsi_percentile(work, count, percents[p]));
    }
    memcpy(work, source, sizeof(int32_t) * count);
    TEST_ASSERT_EQUAL_INT32(sorted[count / 2], statsi_median(work, count));
    free(sorted);
    free(work);
}

static void
test_statsi_percentile_matches_a_full_sort(void) {
    int32_t* values = malloc(sizeof(int32_t) * STATS_MAX);
    TEST_ASSERT_NOT_NULL(values);
    for (size_t count = 1; count <= STATS_MAX; count++) {
        uint32_t state = (uint32_t)count;
        for (size_t i = 0; i < count; i++) {
            state = (state * 1664525U) + 1013904223U;
            values[i] = (int32_t)(state >> 20) - 2048;
        }
        expect_ranks_like_a_sort(values, count);
        for (size_t i = 0; i < count; i++) {
            values[i] = (int32_t)i;
        }
        expect_ranks_like_a_sort(values, count);
        for (size_t i = 0; i < count; i++) {
            values[i] = (int32_t)(count - i);
        }
        expect_ranks_like_a_sort(values, count);
        for (size_t i = 0; i < count; i++) {
            values[i] = 7;
        }
        expect_ranks_like_a_sort(values, count);
    }
    free(values);
}

static void
test_statsf_median_of_odd_and_even_counts(void) {
    float odd[] = {3.5F, -1.0F, 2.0F, 9.0F, 0.5F};
    TEST_ASSERT_EQUAL_FLOAT(2.0F, statsf_median(odd, sizeof odd / sizeof odd[0]));
    float even[] = {4.0F, 1.0F, 3.0F, 2.0F};
    TEST_ASSERT_EQUAL_FLOAT(3.0F, statsf_median(even, sizeof even / sizeof even[0]));
    float one[] = {-6.25F};
    TEST_ASSERT_EQUAL_FLOAT(-6.25F, statsf_percentile(one, 1, 100));
}

void
run_stats_suite(void) {
    RUN_TEST(test_statsi_percentile_matches_a_full_sort);
    RUN_TEST(test_statsf_median_of_odd_and_even_counts);
}

SUITE_REGISTER(run_stats_suite);

/* Capture lengths bound timer rounding independently of the device clock. */
#include "suites.h"
#include "testkit/bench.h"
#include "unity.h"

static void
test_icount_capture_length_boundary(void) {
    const int batches[] = {1, BENCH_ICOUNT_ROUNDING_MARGIN};
    for (size_t i = 0; i < sizeof batches / sizeof batches[0]; i++) {
        const int64_t minimum = (int64_t)batches[i] * BENCH_ICOUNT_ROUNDING_MARGIN * BENCH_ICOUNT_TOLERANCE_DENOMINATOR;
        TEST_ASSERT_FALSE(bench_icount_long_enough(minimum - 1, batches[i]));
        TEST_ASSERT_TRUE(bench_icount_long_enough(minimum, batches[i]));
    }
}

void
suite_perf_stats(void) {
    RUN_TEST(test_icount_capture_length_boundary);
}

SUITE_REGISTER(suite_perf_stats);

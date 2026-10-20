/* Capture lengths bound timer rounding independently of the device clock. */
#include "perf_stats.h"
#include "suites.h"
#include "unity.h"

static void
test_icount_capture_length_boundary(void) {
    const int batches[] = {1, PERF_ICOUNT_ROUNDING_MARGIN};
    for (size_t i = 0; i < sizeof batches / sizeof batches[0]; i++) {
        const int64_t minimum = (int64_t)batches[i] * PERF_ICOUNT_ROUNDING_MARGIN * PERF_ICOUNT_TOLERANCE_DENOMINATOR;
        TEST_ASSERT_FALSE(perf_icount_long_enough(minimum - 1, batches[i]));
        TEST_ASSERT_TRUE(perf_icount_long_enough(minimum, batches[i]));
    }
}

void
suite_perf_stats(void) {
    RUN_TEST(test_icount_capture_length_boundary);
}

SUITE_REGISTER(suite_perf_stats);

/* Portable suite: timing.h's clock, on whichever platform runs it. */

#include <stdint.h>

#include "suites.h"
#include "unity.h"

#include "util/timing.h"

#define READS     1000
/* Far more than a microsecond's work on either platform, so a stuck clock
 * fails here rather than hanging the run. */
#define SPINS_MAX 100000000

static void
test_the_clock_is_positive_and_never_runs_backwards(void) {
    int64_t previous = timing_now_us();
    TEST_ASSERT_GREATER_THAN_INT64(0, previous);
    for (int i = 0; i < READS; i++) {
        const int64_t now = timing_now_us();
        TEST_ASSERT_GREATER_OR_EQUAL_INT64(previous, now);
        previous = now;
    }
}

static void
test_the_clock_advances_while_work_is_done(void) {
    const int64_t started = timing_now_us();
    for (volatile int32_t spin = 0; spin < SPINS_MAX && timing_now_us() == started; spin++) {}
    TEST_ASSERT_GREATER_THAN_INT64(started, timing_now_us());
}

void
suite_timing(void) {
    RUN_TEST(test_the_clock_is_positive_and_never_runs_backwards);
    RUN_TEST(test_the_clock_advances_while_work_is_done);
}

SUITE_REGISTER(suite_timing)

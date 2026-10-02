/*
 * timing.h's clock. Only the board's is monotonic, so only there is it held
 * to never running backwards; a host's is a wall clock, held to counting in
 * microseconds against the C library's own processor clock.
 */

#include <stdint.h>
#include <time.h>

#include "suites.h"
#include "unity.h"

#include "util/timing.h"

/* Far more than a microsecond's work on either platform, so a stuck clock
 * fails here rather than hanging the run. */
#define SPINS_MAX 100000000

static void
test_the_clock_advances_while_work_is_done(void) {
    const int64_t started = timing_now_us();
    TEST_ASSERT_GREATER_THAN_INT64(0, started);
    for (volatile int32_t spin = 0; spin < SPINS_MAX && timing_now_us() == started; spin++) {}
    TEST_ASSERT_GREATER_THAN_INT64(started, timing_now_us());
}

#ifdef DEVICE_BUILD

#define READS    1000
#define SLEEP_MS 20

static void
test_the_clock_never_runs_backwards(void) {
    int64_t previous = timing_now_us();
    for (int i = 0; i < READS; i++) {
        const int64_t now = timing_now_us();
        TEST_ASSERT_GREATER_OR_EQUAL_INT64(previous, now);
        previous = now;
    }
}

static void
test_a_sleep_lasts_at_least_what_it_asks(void) {
    const int64_t started = timing_now_us();
    timing_sleep_ms(SLEEP_MS);
    TEST_ASSERT_GREATER_OR_EQUAL_INT64((int64_t)SLEEP_MS * 1000, timing_now_us() - started);
}

void
suite_timing(void) {
    RUN_TEST(test_the_clock_advances_while_work_is_done);
    RUN_TEST(test_the_clock_never_runs_backwards);
    RUN_TEST(test_a_sleep_lasts_at_least_what_it_asks);
}

#else

/* Spinning keeps wall and processor time close, and the window is long enough
 * that a millisecond or nanosecond clock lands a thousand times outside the
 * bounds; a loaded machine only stretches the wall side, so the upper bound is
 * loose. */
#define SPIN_US     50000
#define SPIN_US_MAX (SPIN_US * 100)

static void
test_the_clock_counts_microseconds(void) {
    const clock_t spin_ticks = (clock_t)((long long)SPIN_US * CLOCKS_PER_SEC / 1000000);
    const int64_t started = timing_now_us();
    const clock_t cpu_started = clock();
    while (clock() - cpu_started < spin_ticks) {}
    const int64_t elapsed = timing_now_us() - started;

    TEST_ASSERT_GREATER_OR_EQUAL_INT64(SPIN_US / 2, elapsed);
    TEST_ASSERT_LESS_OR_EQUAL_INT64(SPIN_US_MAX, elapsed);
}

void
suite_timing(void) {
    RUN_TEST(test_the_clock_advances_while_work_is_done);
    RUN_TEST(test_the_clock_counts_microseconds);
}

#endif

SUITE_REGISTER(suite_timing)

/*
 * On-device self test.
 *
 * Compiled only into a CONFIG_LAUNCHER_SELFTEST build; release carries no
 * test code. With CONFIG_LAUNCHER_SELFTEST_AUTORUN it runs every registered
 * suite at boot, before the launcher starts; otherwise suites run on demand
 * (RUNSUITE on the console, or an on-device toggle).
 *
 * The full run includes the portable suites. Passing on a host proves the
 * logic on a laptop; running them here proves the same code behaves
 * identically built by the Xtensa toolchain and executed on this chip.
 *
 * A full run takes about 18 minutes on the S3, too long for every boot,
 * which is why autorun is opt-in.
 */

#include "selftest/selftest.h"

#include <stdio.h>

#include "esp_log.h"
#include "unity.h"

#include "app/app_arena.h"
#include "core/app_heap.h"
#include "core/memory.h"
#include "core/timing.h"
#include "display/display_shell.h"
#include "suites.h"
#include "test_harness.h"

static const char* TAG = "selftest";

void
__wrap_esp_system_console_put_char(char c) {
    if (c != '\r') {
        (void)putchar((unsigned char)c);
    }
}

/* Unity requires these once per binary. The test ran and was measured in
 * timing.c, where no TEST_PASS() can skip it; tearDown() runs under an
 * abort frame of Unity's own, so it is where the verdict is asserted. Both
 * reset the panel clock to the shell's own state, as an app's start
 * and exit do, so no test inherits what the screen before it or an earlier
 * test set - a failed assert included. A test that leaves app heap blocks
 * behind fails by name, as on a host. */
static size_t app_blocks_before;

void
setUp(void) {
    app_blocks_before = app_heap_blocks();
    display_restore_system_state();
}

void
tearDown(void) {
    display_restore_system_state();
    suite_judge_watched_test();
    /* A shell test leaves the app running the suites, as leaving any app
     * does: the next test runs in a new visit. */
    if (!app_heap_active()) {
        app_arena_enter();
    }
    TEST_ASSERT_EQUAL_size_t_MESSAGE(app_blocks_before, app_heap_blocks(), "test leaked app heap blocks");
}

bool
selftest_visit_begin(void) {
    if (app_heap_active()) {
        return false;
    }
    app_arena_enter();
    return true;
}

void
selftest_visit_end(bool owned) {
    if (owned) {
        app_arena_exit();
    }
}

int
selftest_run(void) {
    const int64_t started = timing_now_us();

    ESP_LOGI(TAG, "running self test");

    const bool own_visit = selftest_visit_begin();
    UNITY_BEGIN();

    /* Every registered suite, portable and hardware alike. Which ones exist
     * is decided at compile time by what was built in; see suites.h. */
    suites_run_all();
    suite_report_frame_watch();

    int failures = UNITY_END();
    selftest_visit_end(own_visit);
    const int64_t elapsed_ms = (timing_now_us() - started) / 1000;

    /* A suite that did not fit is a test that did not run. Folded into the
     * count so the sentinel below (and every harness that reads it) sees a
     * failed run rather than a green one that tested less than it claims. */
    if (suites_dropped() > 0) {
        printf("FAIL: %d registration(s) dropped; the SUITE OVERFLOW lines name them\n", suites_dropped());
        failures += suites_dropped();
    }

    /* A sentinel on its own line, so an automated harness can tell a finished
     * run from a board that went quiet mid-test. */
    printf("\nSELFTEST_COMPLETE failures=%d elapsed_ms=%lld\n", failures, (long long)elapsed_ms);
    (void)fflush(stdout);

    if (failures > 0) {
        ESP_LOGE(TAG, "%d test(s) FAILED", failures);
    } else {
        ESP_LOGI(TAG, "all tests passed in %lld ms", (long long)elapsed_ms);
    }
    return failures;
}

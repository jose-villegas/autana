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
 * A full run takes about 18 minutes on the S3 - too long for every boot,
 * which is why autorun is opt-in.
 */

#include "boot/selftest.h"

#include <stdio.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "unity.h"
#include "unity_test_utils_memory.h"

#include "suites.h"
#include "timing.h"

static const char* TAG = "selftest";

static size_t free_8bit_before;
static size_t free_32bit_before;
static UBaseType_t stack_free_before;

/* Suites run on the main task. An overflow trips FreeRTOS's canary only at a
 * context switch and resets the chip, so a test that leaves less than an
 * interrupt's saved context plus a log line fails by name instead. */
#define STACK_RESERVE_BYTES 512u

void
__wrap_esp_system_console_put_char(char c) {
    if (c != '\r') {
        (void)putchar((unsigned char)c);
    }
}

/* Unity requires these once per binary. The runner owns the memory audit. */
void
setUp(void) {
    free_8bit_before = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    free_32bit_before = heap_caps_get_free_size(MALLOC_CAP_32BIT);
    stack_free_before = uxTaskGetStackHighWaterMark(NULL);
    unity_utils_record_free_mem();
}

void
tearDown(void) {
    suite_run_test_cleanup();
    const UBaseType_t stack_free = uxTaskGetStackHighWaterMark(NULL);
    if (stack_free < stack_free_before) {
        TEST_ASSERT_GREATER_OR_EQUAL_UINT32_MESSAGE(STACK_RESERVE_BYTES, stack_free,
                                                    "the test left the main task's stack almost full");
    }
    if (heap_caps_get_free_size(MALLOC_CAP_8BIT) < free_8bit_before
        || heap_caps_get_free_size(MALLOC_CAP_32BIT) < free_32bit_before) {
        unity_utils_record_free_mem();
        suite_repeat_watched_test();
        suite_run_test_cleanup();
    }
    unity_utils_evaluate_leaks_direct(0);
}

int
selftest_run(void) {
    const int64_t started = esp_timer_get_time();

    ESP_LOGI(TAG, "running self test");

    UNITY_BEGIN();

    /* Every registered suite, portable and hardware alike. Which ones exist
     * is decided at compile time by what was built in - see suites.h. */
    suites_run_all();
    suite_report_frame_watch();

    int failures = UNITY_END();
    const int64_t elapsed_ms = (esp_timer_get_time() - started) / 1000;

    /* A suite that did not fit is a test that did not run. Folded into the
     * count so the sentinel below - and every harness that reads it - sees a
     * failed run rather than a green one that tested less than it claims. */
    if (suites_dropped() > 0) {
        ESP_LOGE(TAG, "%d suite(s) dropped; raise SUITE_MAX in suites.h", suites_dropped());
        failures += suites_dropped();
    }

    /* A sentinel on its own line, so an automated harness can tell a finished
     * run from a board that went quiet mid-test. */
    printf("\nSELFTEST_COMPLETE failures=%d elapsed_ms=%lld\n", failures, (long long)elapsed_ms);
    fflush(stdout);

    if (failures > 0) {
        ESP_LOGE(TAG, "%d test(s) FAILED", failures);
    } else {
        ESP_LOGI(TAG, "all tests passed in %lld ms", (long long)elapsed_ms);
    }
    return failures;
}

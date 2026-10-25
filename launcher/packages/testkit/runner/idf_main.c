/* idf_main: package test and benchmark lifecycle on ESP-IDF. */
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "testkit/cleanup.h"
#include "testkit/suite.h"
#include "unity.h"

void
setUp(void) {
    suite_clear_test_cleanup();
}

void
tearDown(void) {
    suite_run_test_cleanup();
}

static void
run_selected(bool on_request) {
    for (size_t i = 0; i < suites_count(); i++) {
        const suite_entry_t* suite = suites_at(i);
        if (suite->on_request == on_request) {
            suite->fn();
        }
    }
}

void
app_main(void) {
    UNITY_BEGIN();
    run_selected(false);
    run_selected(true);
    const int failures = UNITY_END() + suites_dropped() + (suites_count() == 0);
    printf("TESTKIT_COMPLETE failures=%d\n", failures);
    for (;;) {
        vTaskDelay(portMAX_DELAY);
    }
}

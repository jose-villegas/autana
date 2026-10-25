/* idf_main: package test and benchmark lifecycle on ESP-IDF. */
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "testkit/suite.h"
#include "unity.h"

void
app_main(void) {
    UNITY_BEGIN();
    int errors = suites_run(false);
    errors += suites_run(true);
    const int failures = UNITY_END() + errors;
    printf("TESTKIT_COMPLETE failures=%d\n", failures);
    for (;;) {
        vTaskDelay(portMAX_DELAY);
    }
}

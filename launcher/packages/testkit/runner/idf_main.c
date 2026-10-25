/* idf_main: package test and benchmark lifecycle on ESP-IDF. */
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "testkit/bench.h"
#include "testkit/suite.h"
#include "unity.h"

#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
#include "driver/usb_serial_jtag.h"

#define TESTKIT_CONSOLE_WAIT_MS   30000
#define TESTKIT_CONSOLE_POLL_MS   100
#define TESTKIT_CONSOLE_SETTLE_MS 5000

static void
wait_for_console(void) {
    if (bench_is_qemu()) {
        return;
    }
    const TickType_t start = xTaskGetTickCount();
    while (!usb_serial_jtag_is_connected() && xTaskGetTickCount() - start < pdMS_TO_TICKS(TESTKIT_CONSOLE_WAIT_MS)) {
        vTaskDelay(pdMS_TO_TICKS(TESTKIT_CONSOLE_POLL_MS));
    }
    vTaskDelay(pdMS_TO_TICKS(TESTKIT_CONSOLE_SETTLE_MS));
}
#endif

void
app_main(void) {
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    wait_for_console();
#endif
    printf("TESTKIT_START\n");
    UNITY_BEGIN();
    int errors = suites_run(false);
    errors += suites_run(true);
    const int failures = UNITY_END() + errors;
    printf("TESTKIT_COMPLETE failures=%d\n", failures);
    for (;;) {
        vTaskDelay(portMAX_DELAY);
    }
}

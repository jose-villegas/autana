/* timing_device: timing.h's pause, over the scheduler. */

#include "util/timing.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

void
timing_sleep_ms(uint32_t ms) {
    vTaskDelay(pdMS_TO_TICKS(ms));
}

void
timing_yield(void) {
    vTaskDelay(1);
}

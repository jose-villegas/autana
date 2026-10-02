/* timing_device: timing.h over the high-resolution timer and the scheduler. */

#include "util/timing.h"

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

int64_t
timing_now_us(void) {
    return esp_timer_get_time();
}

void
timing_sleep_ms(uint32_t ms) {
    vTaskDelay(pdMS_TO_TICKS(ms));
}

void
timing_yield(void) {
    vTaskDelay(1);
}

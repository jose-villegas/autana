/* timing_device: timing.h's pause, over the scheduler. */

#include "util/timing.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* A delay of n ticks starts partway through the current tick, so it lasts
 * between n-1 and n tick periods; and the tick need not agree with the clock
 * (under emulation it runs fast). The clock decides when the pause is over. */
void
timing_sleep_ms(uint32_t ms) {
    const int64_t until = timing_now_us() + ((int64_t)ms * 1000);
    vTaskDelay(pdMS_TO_TICKS(ms));
    while (timing_now_us() < until) {
        vTaskDelay(1);
    }
}

void
timing_yield(void) {
    vTaskDelay(1);
}

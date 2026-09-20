#include "util/frame_cost.h"

#if FRAME_COST_ENABLED

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static frame_cost_t shared;
static TaskHandle_t owner_task;
static int foreign_task_calls;

/* The frame loop's own task claims ownership on its first bracket; a begin
 * from any other task is refused rather than corrupt the shared instance. */
int
frame_cost_begin(void) {
    const TaskHandle_t caller = xTaskGetCurrentTaskHandle();
    if (owner_task == NULL) {
        owner_task = caller;
    }
    if (caller != owner_task) {
        foreign_task_calls++;
        return FRAME_COST_IGNORE_MARK;
    }
    return frame_cost_enter(&shared, esp_timer_get_time());
}

void
frame_cost_end(int mark, const char* name) {
    frame_cost_leave(&shared, mark, name, esp_timer_get_time());
}

int
frame_cost_take_report(uint32_t frames, char* out, size_t out_size) {
    const int length = frame_cost_report(&shared, frames, out, out_size);
    if (frames == 0 || out_size == 0 || foreign_task_calls == 0) {
        return length;
    }
    const int wrote = snprintf(out + length, out_size - (size_t)length, " +%d foreign", foreign_task_calls);
    foreign_task_calls = 0;
    if (wrote < 0 || (size_t)(length + wrote) >= out_size) {
        out[length] = '\0';
        return length;
    }
    return length + wrote;
}

#endif

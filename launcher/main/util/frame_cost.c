#include "util/frame_cost.h"

#if FRAME_COST_ENABLED

#include "esp_timer.h"

/* Charged from the frame loop's own task only. */
static frame_cost_t shared;

int64_t
frame_cost_now_us(void) {
    return esp_timer_get_time();
}

void
frame_cost_charge(const char* name, int64_t since_us) {
    frame_cost_add(&shared, name, esp_timer_get_time() - since_us);
}

int
frame_cost_take_report(uint32_t frames, char* out, size_t out_size) {
    return frame_cost_report(&shared, frames, out, out_size);
}

#endif

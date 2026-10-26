#include "profile/frame_laps.h"

#if FRAME_LAPS_ENABLED

#include <string.h>

#include "freertos/FreeRTOS.h"

frame_laps_core_t frame_laps_cores[FRAME_LAPS_CORES];

/* Laps run back to back to time one. */
#define FRAME_LAPS_CALIBRATION_LAPS 256

static const char* names[FRAME_LAPS_NAMES];
static int name_count;
static portMUX_TYPE names_lock = portMUX_INITIALIZER_UNLOCKED;

/* Both cores may meet a name first at once: one adds it, the other finds it. */
frame_laps_slot_t
frame_laps_slot(const char* name) {
    frame_laps_slot_t slot = 0;
    portENTER_CRITICAL(&names_lock);
    for (int i = 0; i < name_count && slot == 0; i++) {
        if (names[i] == name || strcmp(names[i], name) == 0) {
            slot = (frame_laps_slot_t)(i + 1);
        }
    }
    if (slot == 0 && name_count < FRAME_LAPS_NAMES) {
        names[name_count++] = name;
        slot = (frame_laps_slot_t)name_count;
    }
    portEXIT_CRITICAL(&names_lock);
    return slot;
}

/* One lap's own cycles: laps run back to back on scratch state, the way
 * FRAME_LAP() runs them, but for its once-a-call-site slot lookup. */
static uint32_t
lap_cycles(void) {
    static frame_laps_core_t scratch[FRAME_LAPS_CORES];
    const uint32_t from = esp_cpu_get_cycle_count();
    for (int i = 0; i < FRAME_LAPS_CALIBRATION_LAPS; i++) {
        frame_laps_switch(&scratch[esp_cpu_get_core_id()], esp_cpu_get_cycle_count(), (frame_laps_slot_t)(1 + (i & 1)));
    }
    return (esp_cpu_get_cycle_count() - from) / FRAME_LAPS_CALIBRATION_LAPS;
}

int
frame_laps_take_report(uint32_t frames, char* out, size_t out_size) {
    static uint32_t lap;
    if (lap == 0) {
        lap = lap_cycles();
    }
    return frame_laps_report(frame_laps_cores, FRAME_LAPS_CORES, names, frames, CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ, lap,
                             out, out_size);
}

#endif

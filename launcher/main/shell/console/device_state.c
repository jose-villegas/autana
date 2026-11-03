/*
 * Read a board snapshot for the console screenshot state.
 */
#include "shell/console/device_state.h"

#include "sdkconfig.h"

#include "board/board.h"
#include "core/memory.h"
#include "core/timing.h"
#include "display/display.h"

void
device_state_read(device_state_t* out) {
    out->uptime_us = timing_now_us();
    out->heap_free_bytes = (uint32_t)memory_free_bytes(MEMORY_8BIT);
    out->heap_min_free_bytes = (uint32_t)memory_min_free_bytes(MEMORY_8BIT);

    /* Not a runtime query: this project does not enable dynamic frequency
     * scaling (no CONFIG_PM_ENABLE), so the Kconfig value already IS the
     * running frequency, and reading it back at runtime would need
     * esp_clk_cpu_freq() from esp_hw_support's private
     * esp_private/esp_clk.h, not a header this module should reach into
     * for a value that cannot actually change on this board. */
    out->cpu_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;

    out->temp_ok = board_temp_sensor_read_celsius(&out->temp_c) == BOARD_TEMP_SENSOR_OK;

    out->quarter = display_quarter_now();

    out->imu_ready = imu_ready();
    out->imu_read_ok = out->imu_ready && imu_read(&out->imu, NULL);
}

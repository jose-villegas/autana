/*
 * device_state.c - the device-only half: everything device_state_read()
 * needs actual hardware for. See device_state.h for the pure formatting
 * side and the module's own reason to exist.
 */
#include "util/device_state.h"

#include "esp_system.h"
#include "esp_timer.h"

#include "board/board.h"
#include "display/display.h"

void
device_state_read(device_state_t* out) {
    out->uptime_us = esp_timer_get_time();
    out->heap_free_bytes = (uint32_t)esp_get_free_heap_size();
    out->heap_min_free_bytes = (uint32_t)esp_get_minimum_free_heap_size();

    /* Not a runtime query: this project does not enable dynamic frequency
     * scaling (no CONFIG_PM_ENABLE), so the Kconfig value already IS the
     * running frequency, and reading it back at runtime would need
     * esp_clk_cpu_freq() from esp_hw_support's private
     * esp_private/esp_clk.h - not a header this module should reach into
     * for a value that cannot actually change on this board. */
    out->cpu_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;

    out->temp_ok = board_temp_sensor_read_celsius(&out->temp_c) == TEMP_SENSOR_OK;

    out->quarter = display_shell_quarter();

    out->imu_ready = imu_ready();
    out->imu_read_ok = out->imu_ready && imu_read(&out->imu);
}

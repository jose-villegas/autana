/*
 * Board state for console screenshots. The reader samples hardware; the
 * inline formatter accepts a snapshot and frame input without I/O.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "input/imu.h"
#include "input/input.h"

typedef struct {
    int64_t uptime_us; /* esp_timer_get_time() */
    uint32_t heap_free_bytes;
    uint32_t heap_min_free_bytes; /* low-water mark since boot */
    int cpu_freq_mhz;

    bool temp_ok; /* false if the sensor did not answer */
    float temp_c; /* only meaningful if temp_ok */

    int quarter; /* display_shell_quarter() */

    bool imu_ready;   /* false: chip absent/never initialised */
    bool imu_read_ok; /* only meaningful if imu_ready */
    imu_sample_t imu; /* only meaningful if imu_ready && imu_read_ok */
} device_state_t;

/* Reads everything above fresh - see device_state.c. Touches hardware, so
 * this is not something to call every frame - fine for an occasional,
 * deliberately-triggered snapshot like console_screenshot_dump()'s. */
void device_state_read(device_state_t* out);

/* Large enough for every field at its worst-case width (a full int64_t
 * uptime, both heap counters at UINT32_MAX, the IMU's six int16_t axes all
 * at their most negative, touch/button booleans and coordinates) with
 * comfortable headroom - see device_state_format_json() below for the
 * shape being budgeted against. */
#define DEVICE_STATE_JSON_MAX 512

/* Formats `state` (already read by device_state_read()) plus a frame's
 * `input` as one line of JSON into `out[DEVICE_STATE_JSON_MAX]`. Always
 * NUL-terminated; snprintf() truncates rather than overflows if the
 * budgeted shape does not fit.
 *
 * `input` is passed in, not read here: this function has no notion of "the
 * current frame" on its own - see console_screenshot_dump()'s own comment
 * (console/console_screenshot.h) for why its caller passes the exact
 * input_t the frame being captured was drawn with. */
static inline void
device_state_format_json(const device_state_t* state, const input_t* input, char out[DEVICE_STATE_JSON_MAX]) {
    char imu_json[96];
    if (!state->imu_ready) {
        snprintf(imu_json, sizeof imu_json, "{\"ready\":false}");
    } else if (!state->imu_read_ok) {
        snprintf(imu_json, sizeof imu_json, "{\"ready\":true,\"read_failed\":true}");
    } else {
        /* Raw counts, same units imu.h itself leaves them in (4096/g,
         * 64/dps) - IMU_COUNTS_PER_G/IMU_COUNTS_PER_DPS in imu.h are the
         * conversion for whoever reading the dump wants physical units. */
        snprintf(imu_json, sizeof imu_json,
                 "{\"ready\":true,\"ax\":%d,\"ay\":%d,\"az\":%d,"
                 "\"gx\":%d,\"gy\":%d,\"gz\":%d}",
                 state->imu.ax, state->imu.ay, state->imu.az, state->imu.gx, state->imu.gy, state->imu.gz);
    }

    char temp_json[24];
    if (state->temp_ok) {
        snprintf(temp_json, sizeof temp_json, "%.1f", (double)state->temp_c);
    } else {
        snprintf(temp_json, sizeof temp_json, "null");
    }

    snprintf(out, DEVICE_STATE_JSON_MAX,
             "{\"uptime_us\":%lld,\"heap_free_bytes\":%u,"
             "\"heap_min_free_bytes\":%u,\"cpu_freq_mhz\":%d,"
             "\"temp_c\":%s,\"orientation_quarter\":%d,\"imu\":%s,"
             "\"touch\":{\"down\":%s,\"x\":%d,\"y\":%d},"
             "\"buttons\":{\"boot_down\":%s,\"power_held\":%s}}",
             (long long)state->uptime_us, (unsigned)state->heap_free_bytes, (unsigned)state->heap_min_free_bytes,
             state->cpu_freq_mhz, temp_json, state->quarter, imu_json, input->down ? "true" : "false", input->x,
             input->y, input->boot.down ? "true" : "false", input->power.held ? "true" : "false");
}

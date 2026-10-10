/*
 * timing: the clock and the pause, so a caller above the drivers asks for
 * time without naming the scheduler. The pause is timing_device.c's, on the
 * board only.
 *
 * The clock is read on a frame's hottest paths (frame_cost's stages), so it
 * is inline: on the board a read is the one call to the high-resolution
 * timer it would be without this header. A host reads standard C's wall
 * clock: enough for a test's elapsed time, but it can step, so only the
 * board's clock is monotonic.
 */
#pragma once

#include <stdint.h>

#ifdef ESP_PLATFORM
#include "esp_timer.h"
#else
#include <time.h>
#endif

/* Microseconds since boot, monotonic. On a host, wall-clock microseconds
 * since the epoch. */
static inline __attribute__((always_inline)) int64_t
timing_now_us(void) {
#ifdef ESP_PLATFORM
    return esp_timer_get_time();
#else
    struct timespec now;
    (void)timespec_get(&now, TIME_UTC);
    return (int64_t)now.tv_sec * 1000000 + now.tv_nsec / 1000;
#endif
}

/* Blocks the calling task for at least `ms`, letting others run. */
void timing_sleep_ms(uint32_t ms);

/* Gives up the rest of this scheduler tick: enough for the idle task to feed
 * the watchdog, without a wait worth naming. */
void timing_yield(void);

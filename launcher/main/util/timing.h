/*
 * timing: the clock and the pause, so a caller above the drivers asks for
 * time without naming the scheduler. Defined in timing_device.c, device only.
 */
#pragma once

#include <stdint.h>

/* Microseconds since boot, monotonic. */
int64_t timing_now_us(void);

/* Blocks the calling task for at least `ms`, letting others run. */
void timing_sleep_ms(uint32_t ms);

/* Gives up the rest of this scheduler tick: enough for the idle task to feed
 * the watchdog, without a wait worth naming. */
void timing_yield(void);

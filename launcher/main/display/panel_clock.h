/*
 * panel_clock - the shell's memory of the system panel clock, the user's
 * choice, and the rule that every app switch returns to it whatever the
 * previous app set.
 *
 * Pure state, no gfx and no NVS, so a host suite drives it; main.c reads
 * the saved choice, applies what this returns and saves what it accepts.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define PANEL_CLOCK_SLOW_HZ (40 * 1000 * 1000)
#define PANEL_CLOCK_FAST_HZ (80 * 1000 * 1000)

typedef struct {
    int system_hz;
} panel_clock_t;

/* `found` is whether a choice was saved at all; anything but a valid saved
 * rate falls back to `default_hz`. */
void panel_clock_init(panel_clock_t* p, bool found, int32_t saved_hz, int default_hz);

/* Returns false, changing nothing, for a rate the link cannot run. */
bool panel_clock_set_system(panel_clock_t* p, int hz);

int panel_clock_system_hz(const panel_clock_t* p);

/* The clock to apply as an app starts or exits. */
int panel_clock_for_switch(const panel_clock_t* p);

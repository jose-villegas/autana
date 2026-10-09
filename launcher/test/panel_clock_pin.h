/* A timed present measures a known panel clock, not the system's choice of
 * one. The selftest runner's tearDown() puts the system clock back. */
#pragma once

#include "gfx/present/gfx_present.h"

/* Presents once at hz, so the link reopen lands outside any timed window. */
static inline void
panel_clock_pin(int hz) {
    gfx_set_panel_clock_hz(hz);
    gfx_present();
}

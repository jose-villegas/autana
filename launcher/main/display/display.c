/*
 * display: see display.h for the module's job and the hysteresis math.
 *
 * A real translation unit rather than static inline in the header (compare
 * ui_style.h/ui_transform.h, which are header-only because their geometry is
 * a handful of one-shot computations with no state of their own to carry
 * between calls). This module is a small state machine: the decision on
 * one call depends on where display_update() left `quarter` last time, so
 * it gets the same treatment as gesture.c and tilt.c, the two modules this
 * one is explicitly modelled on.
 */

#include "display/display.h"
#include "display/display_shell.h"

#include <math.h>
#include <stdint.h>

int
display_panel_corner_inset(int radius, int canvas_height, int row) {
    if (radius <= 0 || row < 0 || row >= canvas_height) {
        return 0;
    }
    const int from_nearest_edge = row < canvas_height - 1 - row ? row : canvas_height - 1 - row;
    if (from_nearest_edge >= radius) {
        return 0;
    }
    const int up = radius - from_nearest_edge;
    return radius - (int)lroundf(sqrtf((float)((radius * radius) - (up * up))));
}

void
display_init(display_t* d) {
    /* 0, always: a neutral reset with no opinion about which way the
     * board is actually held. DISPLAY_DEFAULT_QUARTER (display.h) is a
     * physical fact about THIS shell's board, not something a
     * device-agnostic module should bake into its own idea of "reset";
     * display_orientation_init() applies it, right after this call. */
    d->quarter = 0;
}

/* Splits (gx, gy) into the component along quarter `q`'s own "down"
 * direction and the component perpendicular to it; see display.h's top
 * comment. Mirrors the branch structure of picking a quarter from scratch:
 * quarters 0/2 read gy as the deciding axis and gx as the offender; 1/3 the
 * other way round. `*aligned` is positive when the board is still roughly
 * where quarter `q` expects. */
static void
split_gravity(int q, int gx, int gy, int* aligned, int* perp) {
    switch (q) {
        case 0:
            *aligned = gy;
            *perp = gx;
            break; /* down is down */
        case 2:
            *aligned = -gy;
            *perp = gx;
            break; /* board upside down */
        case 3:
            *aligned = gx;
            *perp = gy;
            break;       /* down is to the right */
        default: /* 1 */ /* down is to the left */
            *aligned = -gx;
            *perp = gy;
            break;
    }
}

/* Which quarter is reached by leaving `q` when `perp` (the perpendicular
 * component split_gravity() just computed for `q`) is the one driving the
 * switch, entered from whichever quarter is already current instead of
 * recomputed from nothing every call. */
static int
neighbor_quarter(int q, int perp) {
    if (q == 0 || q == 2) {
        return (perp >= 0) ? 3 : 1;
    }
    return (perp >= 0) ? 0 : 2;
}

bool
display_update(display_t* d, vec2i_t gravity) {
    int aligned, perp;
    split_gravity(d->quarter, gravity.x, gravity.y, &aligned, &perp);

    const int perp_abs = (perp < 0) ? -perp : perp;

    /* Cross-multiplied instead of divided: exact integer arithmetic, no
     * rounding, and no need to guard a zero denominator. See display.h's top
     * comment for the ratio, the angle it stands in for, and why a negative
     * `aligned` (tilt past 90 degrees from the current quarter) falls
     * straight through to a switch instead of getting stuck: the right side
     * goes negative while perp_abs stays non-negative, so the "no switch"
     * branch below can never be taken. */
    if ((int64_t)perp_abs * DISPLAY_HYST_DEN <= (int64_t)aligned * DISPLAY_HYST_NUM) {
        return false;
    }

    /* neighbor_quarter() always returns something other than d->quarter by
     * construction: each of the four cases above maps to one of the other
     * three, so reaching here always is a real change. */
    d->quarter = neighbor_quarter(d->quarter, perp);
    return true;
}

int
display_quarter(const display_t* d) {
    return d->quarter;
}

void
display_orientation_init(display_orientation_t* o) {
    display_init(&o->display);
    o->display.quarter = DISPLAY_DEFAULT_QUARTER;
    o->next_sample_us = 0;
}

bool
display_orientation_sample(display_orientation_t* o, int64_t now_us, display_motion_reader_t read) {
    if (now_us < o->next_sample_us) {
        return false;
    }
    o->next_sample_us = now_us + (int64_t)DISPLAY_SAMPLE_MS * 1000;

    imu_sample_t sample;
    if (!read(&sample)) {
        return false;
    }
    return display_update(&o->display, imu_gravity_screen(&sample));
}

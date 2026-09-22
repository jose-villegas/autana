/*
 * input - the input layer's own shared type.
 *
 * input_t lives here, not in app.h, so touch.h/touch_fsm.h/gesture.h can
 * declare against it without reaching up into the apps layer - app.h
 * includes this header instead of defining the type itself.
 */
#pragma once

#include <stdbool.h>

#include "input/buttons.h"

/* Touch state for the current frame.
 *
 * `pressed` and `released` are edges (true only on the frame the transition
 * happened); `down` is the level. Edges are what UI code almost always wants -
 * using the level for a button would re-trigger it every frame it is held. */
typedef struct {
    bool down;
    bool pressed;
    bool released;
    int x, y;             /* current position, or the last one seen */
    int press_x, press_y; /* where the current touch began */

    /* The two physical buttons, delivered the same way touch is so an app
     * never has to poll anything itself. See buttons.h - PWR is an event from
     * the power-management chip, so only its `pressed` edge is meaningful. */
    button_t boot;
    button_t power;
} input_t;

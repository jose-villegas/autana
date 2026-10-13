/*
 * input: touch, buttons and motion. Touch and buttons arrive once a frame
 * in input_t; motion is read through input_read_motion(). input_t lives below
 * app/app.h so no input/ header includes upward. The shell's calls that
 * fill it are in input_shell.h.
 */
#pragma once

#include <stdbool.h>

#include "input/buttons.h"
#include "math/linear/vec3i.h"

/* Touch state for the current frame.
 *
 * `pressed` and `released` are edges (true only on the frame the transition
 * happened); `down` is the level. Edges are what UI code almost always wants;
 * using the level for a button would re-trigger it every frame it is held. */
typedef struct {
    bool down;
    bool pressed;
    bool released;
    int x, y;             /* current position, or the last one seen */
    int press_x, press_y; /* where the current touch began */

    /* The two physical buttons, delivered the same way touch is so an app
     * never has to poll anything itself. See buttons.h: PWR is an event from
     * the power-management chip, so only its `pressed` edge is meaningful. */
    button_t boot;
    button_t power;
} input_t;

/* One g of acceleration: input_motion_t's acceleration is Q12 g. */
#define INPUT_ACCEL_ONE_G 4096

/* `acceleration` is the raw accelerometer: gravity plus whatever else moves
 * the board, unfiltered (tilt_t derives a steady gravity from it). Screen
 * axes, pointing the way down at rest: x right, y down, z out of the screen,
 * so upright reads +1 g on y and flat, screen up, -1 g on z. `turn_speed`
 * is how fast the board turns, in deg/s: |x|+|y|+|z| of the gyroscope's
 * rates, unsigned, with no axis. */
typedef struct {
    vec3i_t acceleration;
    int turn_speed;
} input_motion_t;

/* One sensor read (an I2C transaction: call it only in frames that need it).
 * False when there is no sensor or no reading. */
bool input_read_motion(input_motion_t* out);

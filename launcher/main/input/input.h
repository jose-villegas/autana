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
#define INPUT_ACCEL_ONE_G      4096
#define INPUT_ROTATION_ONE_DPS 64

/* Offset-calibrated, unfiltered acceleration: Q12 g in screen axes (x right, y down, z out).
 * Rotation: Q6 deg/s, x=-gy, y=gx, z=gz. Measured right-hand sense:
 * +z clockwise from front, +x bottom edge toward viewer, +y right edge away.
 * turn_speed is unsigned |gx|+|gy|+|gz| in deg/s. */
typedef struct {
    vec3i_t acceleration;
    int turn_speed;
    vec3i_t rotation;
} input_motion_t;

#define INPUT_MOTION_SAMPLE_US 4460U
#define INPUT_MOTION_SAMPLES   128

typedef struct {
    input_motion_t samples[INPUT_MOTION_SAMPLES]; /* oldest first, INPUT_MOTION_SAMPLE_US apart */
    int count;                                    /* zero when there is no new sample */
    bool gap;                                     /* samples were lost before samples[0] */
} input_motion_batch_t;

/* Every sample since the last call. False when there is no sensor or the read failed. */
bool input_read_motion_batch(input_motion_batch_t* out);

/* One sensor read (an I2C transaction: call it only in frames that need it).
 * False when there is no sensor or no reading. */
bool input_read_motion(input_motion_t* out);

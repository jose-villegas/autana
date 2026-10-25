/*
 * input: touch, buttons and motion. Touch and buttons arrive once a frame
 * in input_t; motion is read through input_read_motion(). input_t lives below
 * app/app.h so no input/ header includes upward. The shell's calls that
 * fill it are in input_shell.h.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "input/buttons/buttons.h"
#include "math/linear/vec3f.h"
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

/* Must equal IMU_SAMPLE_US; checked in input_device.c. */
#define INPUT_MOTION_SAMPLE_US 4460U
/* Must equal IMU_FIFO_SAMPLES; checked in input_device.c. */
#define INPUT_MOTION_SAMPLES   128

typedef struct {
    input_motion_t samples[INPUT_MOTION_SAMPLES]; /* oldest first, INPUT_MOTION_SAMPLE_US apart */
    uint8_t lost_before[INPUT_MOTION_SAMPLES];    /* samples lost in FIFO read mode just before this sample */
    int count;                                    /* zero when there is no new sample */
    bool gap;                                     /* overflow or failed drain; correction must restart */
} input_motion_batch_t;

/* One consumer takes queued samples, oldest first. False when there is no sensor. */
bool input_read_motion_batch(input_motion_batch_t* out);

/* Copies the newest sample from memory.
 * False when there is no sensor or no reading. */
bool input_read_motion(input_motion_t* out);

/* Frame thread only. Screen axes (x right, y down, z out), in g:
 * unit while held, zero in free fall; no sensor gives (0,1,0).
 * The first gravity or shake read each frame updates both; later reads are cached. */
vec3f_t input_gravity(void);
/* Frame thread only. Deliberate shake, 0..1: zero below SHAKE_START_G,
 * full at SHAKE_FULL_G; no sensor gives zero. */
float input_shake(void);

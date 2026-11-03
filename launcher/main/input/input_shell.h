/*
 * input_shell: the shell's side of input: starting the touch, button and
 * motion drivers, reading touch and buttons each frame into the input_t an
 * app is handed, and reading acceleration in screen-axis g on demand.
 * An app never calls these. Defined in input_device.c, device only.
 */
#pragma once

#include <stdbool.h>

#include "input/input.h"
#include "math/linear/vec3f.h"

/* Starts the touch and button polling tasks and the motion sensor. A missing
 * sensor is logged and leaves input_read_motion() reporting nothing. */
void input_start(void);

/* Fills `out`'s touch and buttons for this frame and clears the edges they
 * latched. */
void input_poll(input_t* out);

/* Reads motion and fills acceleration in screen-axis g. False when there is no
 * sensor or no reading. */
bool input_read_acceleration(vec3f_t* acceleration);

/* The Q12 g screen-axis offset subtracted on every motion read. */
vec3i_t input_accel_offset(void);
/* Stores the offset and applies it from the next input_read_motion(); zero clears. False, leaving the live
 * offset unchanged, if an axis exceeds IMU_CALIB_OFFSET_MAX or a write fails. */
bool input_set_accel_offset(vec3i_t offset);

/* The Q6 deg/s screen-axis offset subtracted on every motion read. */
vec3i_t input_gyro_offset(void);

/*
 * input_shell: the shell's side of input: starting the touch, button and
 * motion drivers, reading touch and buttons each frame into the input_t an
 * app is handed, and reading gravity in screen axes on demand. System
 * calibration controls also live here. Defined in input_device.c, device only.
 */
#pragma once

#include <stdbool.h>

#include "input/input.h"
#include "math/linear/vec2i.h"

/* Starts the touch and button polling tasks and the motion sensor. A missing
 * sensor is logged and leaves input_read_motion() reporting nothing. */
void input_start(void);

/* Fills `out`'s touch and buttons for this frame and clears the edges they
 * latched. */
void input_poll(input_t* out);

/* Reads motion and fills `gravity` in screen axes. False when there is no
 * sensor or no reading. */
bool input_read_gravity(vec2i_t* gravity);

/* The Q12 g screen-axis offset subtracted on every motion read. */
vec3i_t input_accel_offset(void);
/* Saves and applies on the next read; zero clears. False if invalid or not saved. */
bool input_set_accel_offset(vec3i_t offset);

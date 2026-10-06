/*
 * input_shell: the shell's side of input: starting the touch, button and
 * motion drivers and reading touch and buttons each frame into the input_t an
 * app is handed. An app never calls these. Defined in input_device.c, device
 * only.
 */
#pragma once

#include <stdbool.h>

#include "input/input.h"
#include "util/math/vec2i.h"

/* Starts the touch and button polling tasks and the motion sensor. A missing
 * sensor is logged and leaves imu_read() reporting nothing. */
void input_start(void);

/* Fills `out`'s touch and buttons for this frame and clears the edges they
 * latched. */
void input_poll(input_t* out);

/* Reads the motion sensor and fills `gravity` in screen axes
 * (imu_gravity_screen()). False when there is no sensor or no reading. */
bool input_read_gravity(vec2i_t* gravity);

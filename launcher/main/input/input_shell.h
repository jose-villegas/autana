/*
 * input_shell: the shell's side of input: starting the touch, button and
 * motion drivers and reading them each frame into the input_t an app is
 * handed. An app never calls these. Defined in input_device.c, device only,
 * except input_gesture_name().
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "build_variant.h"
#include "input/imu_sample.h"
#include "input/input.h"

/* Starts the touch and button polling tasks and the motion sensor. A missing
 * sensor is logged and leaves input_read_motion() reporting nothing. */
void input_start(void);

/* Fills `out`'s touch and buttons for this frame and clears the edges they
 * latched. */
void input_poll(input_t* out);

/* Reads the motion sensor. False when there is none or the read failed,
 * leaving `out` untouched. */
bool input_read_motion(imu_sample_t* out);

/* The gesture an injected touch finished. */
typedef enum {
    INPUT_GESTURE_NONE,
    INPUT_GESTURE_TAP,
    INPUT_GESTURE_PRESS,
    INPUT_GESTURE_DRAG,
} input_gesture_t;

/* "TAP", "PRESS" or "DRAG"; NULL for none. */
const char* input_gesture_name(input_gesture_t gesture);

#if CONFIG_LAUNCHER_DEVELOPMENT
/* Touch samples since the last call that carried a point, and how many of
 * those moved: the controller's real report rate. */
void input_take_touch_sample_counts(uint32_t* points, uint32_t* moved);

/* The gesture that finished since the last call, or INPUT_GESTURE_NONE. */
input_gesture_t input_take_gesture_completion(void);
#endif

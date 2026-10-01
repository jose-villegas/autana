/*
 * input: input_t, one frame's touch and buttons, and the three drivers that
 * fill it and the motion sensor beside it. It lives below app.h so no
 * input/ header includes upward. The calls at the foot are defined in
 * input_device.c, so a host build links input_t alone.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "build_variant.h"
#include "input/buttons.h"
#include "input/imu_sample.h"

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

/* Starts the touch and button polling tasks and the motion sensor. A missing
 * sensor is logged and leaves input_read_motion() reporting nothing. */
void input_start(void);

/* Fills `out`'s touch and buttons for this frame and clears the edges they
 * latched. */
void input_poll(input_t* out);

/* Reads the motion sensor. False when there is none or the read failed,
 * leaving `out` untouched. */
bool input_read_motion(imu_sample_t* out);

#if CONFIG_LAUNCHER_DEVELOPMENT
/* Touch samples since the last call that carried a point, and how many of
 * those moved: the controller's real report rate. */
void input_take_touch_sample_counts(uint32_t* points, uint32_t* moved);

/* The name of the injected gesture that finished since the last call, "TAP",
 * "PRESS" or "DRAG", or NULL when none did. */
const char* input_take_gesture_completion_name(void);
#endif

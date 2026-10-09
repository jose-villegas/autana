/*
 * orbit_motion_touch: one finger on the panel turned into orbit_motion
 * steering. A drag turns whatever orbits with the finger, as if spinning a
 * turntable; a double tap asks for home. A touch that lands in the strip along the home-gesture edge
 * or the edge opposite it (where the control centre opens) belongs to the
 * shell, so it never steers or taps.
 *
 * Pure: the frame's input_t, the panel's size and turn, and which edge holds
 * the home gesture come in; orbit_motion.h's orbit_motion_input_t comes out.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "input/gesture.h"
#include "input/input.h"
#include "render/viewport.h"
#include "util/motion/orbit_motion.h"

/* A drag across the picture's shorter side turns the orbit this far. */
#define ORBIT_MOTION_TOUCH_TURN_PER_SHORT_SIDE MATH_PI

/* A touch that strays no further than this from where it landed, and lifts
 * within ORBIT_MOTION_TOUCH_TAP_MAX_MS, is a tap rather than a drag. */
#define ORBIT_MOTION_TOUCH_TAP_SLOP_PX         12
#define ORBIT_MOTION_TOUCH_TAP_MAX_MS          250

/* Two taps whose lifts are at most this far apart are a double tap. */
#define ORBIT_MOTION_TOUCH_DOUBLE_TAP_MS       350

typedef struct {
    bool steering;         /* the finger down now landed outside the shell's strips */
    int last_x, last_y;    /* where it was last frame, panel pixels */
    uint32_t down_ms;      /* how long it has been down */
    bool strayed;          /* it has moved past ORBIT_MOTION_TOUCH_TAP_SLOP_PX */
    bool tapped;           /* a tap ended ORBIT_MOTION_TOUCH_DOUBLE_TAP_MS or less ago */
    uint32_t since_tap_ms; /* how long ago, while `tapped` */
} orbit_motion_touch_t;

/* A finger-free start. */
void orbit_motion_touch_init(orbit_motion_touch_t* touch);

/* This frame's steering, dt_ms after the last call. `viewport` is the panel
 * and its quarter, so a drag turns the picture the way it is held; the
 * strips are measured on the panel, as gesture.h measures them. */
orbit_motion_input_t orbit_motion_touch_step(orbit_motion_touch_t* touch, const input_t* input, uint32_t dt_ms,
                                             viewport_t viewport, gesture_edge_t home_edge);

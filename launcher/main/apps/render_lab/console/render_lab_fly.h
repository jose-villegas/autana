/*
 * render_lab_fly: the showing scene's camera flown from a desktop's keyboard
 * and mouse, over the console's "render fly" lines (tools/fly.py sends them).
 *
 * A steering line is a level, as a TOUCH or IMU line is: its keys stay held
 * until the next line, its drag and wheel add up until the next frame takes
 * them. The first line pauses the camera's clip and flies from where it is;
 * "release", a scene switch, leaving the app, or RENDER_LAB_FLY_IDLE_MS with
 * no line puts the camera back where the flight began and its clip as it was.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "build/build_variant.h"
#include "console/console_number.h"
#include "kinematics/fly/fly_motion.h"
#include "math/geometry/box3f.h"

/* A flight with no line for this long ends, so a pulled cable never leaves
 * the camera flying on the last keys held. */
#define RENDER_LAB_FLY_IDLE_MS    1000

/* A flight starts at the speed that crosses the drawn scene's box, corner to
 * corner, in this many seconds, whatever units the scene is in. */
#define RENDER_LAB_FLY_CROSSING_S 20.0F

/* A steering line's numbers: three key axes, then the drag's two, then the wheel. */
#define RENDER_LAB_FLY_AXES       3
#define RENDER_LAB_FLY_NUMBERS    (RENDER_LAB_FLY_AXES + 3)

/* Reads "<right> <up> <forward> <drag x> <drag y> <wheel>": each key axis -1,
 * 0 or 1, the drag in whole pixels, the wheel in whole notches. False, and
 * nothing written, for a malformed line. */
static inline bool
render_lab_fly_parse(const char* args, fly_motion_steer_t* out) {
    int32_t v[RENDER_LAB_FLY_NUMBERS];
    for (int i = 0; i < RENDER_LAB_FLY_NUMBERS; i++) {
        if (!console_i32(&args, &v[i]) || (i < RENDER_LAB_FLY_AXES && (v[i] < -1 || v[i] > 1))) {
            return false;
        }
    }
    if (*args != '\0') {
        return false;
    }
    *out = (fly_motion_steer_t){
        .move = {(float)v[0], (float)v[1], (float)v[2]},
        .look = {(float)v[3], (float)v[4]},
        .wheel = (float)v[5],
    };
    return true;
}

/* The speed a flight over `bounds` starts at: RENDER_LAB_FLY_CROSSING_S
 * corner to corner, or FLY_MOTION_START_SPEED over an empty box. */
static inline float
render_lab_fly_start_speed(box3f_t bounds) {
    if (box3f_is_empty(bounds)) {
        return FLY_MOTION_START_SPEED;
    }
    return vec3f_length(vec3f_sub(bounds.hi, bounds.lo)) / RENDER_LAB_FLY_CROSSING_S;
}

#if BUILD_VARIANT_DEVELOPMENT_OR_HOST
/* "fly <six numbers>", "fly pose" or "fly release": the words after
 * "render ". False for any other line. */
bool render_lab_fly_console(const char* args);

/* Each pass, after the scene's own steer and update and before it draws:
 * steps the flight, if there is one. */
void render_lab_fly_frame(uint32_t dt_ms);

/* Ends a flight, if there is one: the camera goes back and its clip is left
 * paused or playing, as it was when the flight began. */
void render_lab_fly_stop(void);
#else
/* A release build has no console to fly from. */
static inline bool
render_lab_fly_console(const char* args) {
    (void)args;
    return false;
}

static inline void
render_lab_fly_frame(uint32_t dt_ms) {
    (void)dt_ms;
}

static inline void
render_lab_fly_stop(void) {}
#endif

/*
 * fly_motion: a transform flown freely, as a free camera is: keys move it
 * along its own right and forward and the world's up at its speed, a drag
 * turns it with a level horizon (yaw about the world's up, pitch short of
 * straight up or down, no roll), and a wheel scales its speed.
 *
 * Pure: steering arrives as key axes, pixels and notches, already gathered
 * by whatever reads the input, and the result is a transformf_t the caller
 * writes into its entity.
 */
#pragma once

#include <stdbool.h>

#include "math/linear/transformf.h"
#include "math/linear/vec2f.h"
#include "math/linear/vec3f.h"
#include "math/scalar/math_const.h"

/* A start speed in metres per second, for a scene measured in metres. */
#define FLY_MOTION_START_SPEED       4.0F
/* One wheel notch up multiplies the speed by this; one down divides by it. */
#define FLY_MOTION_SPEED_STEP        1.25F
/* The wheel keeps the speed within these multiples of the start speed. */
#define FLY_MOTION_SPEED_MIN_RATIO   0.0125F
#define FLY_MOTION_SPEED_MAX_RATIO   125.0F
/* Radians a pixel of drag turns. */
#define FLY_MOTION_RADIANS_PER_PIXEL 0.004F
/* The steepest pitch either way: short of straight, where forward would meet the world's up. */
#define FLY_MOTION_PITCH_LIMIT       ((MATH_TAU / 4.0F) - 0.01F)

/* How a flight answers its steering, one per flight, so two flyers can feel different. */
typedef struct {
    float speed_step;                       /* one wheel notch up multiplies the speed by this */
    float speed_min_ratio, speed_max_ratio; /* the wheel's bounds, as multiples of the start speed */
    float radians_per_pixel;                /* turn per pixel of drag */
    float pitch_limit;                      /* steepest pitch either way, at most FLY_MOTION_PITCH_LIMIT */
} fly_motion_tuning_t;

/* The tuning a flight takes when its caller has no other. */
#define FLY_MOTION_TUNING_DEFAULT                                                                                      \
    ((fly_motion_tuning_t){                                                                                            \
        .speed_step = FLY_MOTION_SPEED_STEP,                                                                           \
        .speed_min_ratio = FLY_MOTION_SPEED_MIN_RATIO,                                                                 \
        .speed_max_ratio = FLY_MOTION_SPEED_MAX_RATIO,                                                                 \
        .radians_per_pixel = FLY_MOTION_RADIANS_PER_PIXEL,                                                             \
        .pitch_limit = FLY_MOTION_PITCH_LIMIT,                                                                         \
    })

typedef struct {
    transformf_t pose;
    float speed;       /* scene units per second */
    float start_speed; /* what the wheel's bounds are multiples of */
    fly_motion_tuning_t tuning;
} fly_motion_t;

/* One step of steering. `move` is each -1, 0 or 1: x along the transform's
 * right, y along the world's up, z along its forward. `look` is the drag in
 * pixels, right and down; `wheel` is notches, up positive. */
typedef struct {
    vec3f_t move;
    vec2f_t look;
    float wheel;
} fly_motion_steer_t;

/* Flying from `pose` at `speed`, in the scene's units per second, as
 * `tuning` says; its pitch limit is held to FLY_MOTION_PITCH_LIMIT. The
 * first turn drops the pose's roll and scale. */
fly_motion_t fly_motion_start(transformf_t pose, float speed, fly_motion_tuning_t tuning);

/* Scales the speed by the wheel, turns by the drag, then moves at the speed
 * for `dt_s` seconds, no faster along a diagonal. True when the pose changed. */
bool fly_motion_update(fly_motion_t* fly, const fly_motion_steer_t* steer, float dt_s);

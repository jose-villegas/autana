/*
 * orbit_motion: moves a transform around a target, turntable style: it turns
 * about the world's up axis (yaw) and tilts above or below the target
 * (pitch), always facing it. Steering turns it directly; let go and it
 * keeps turning, slowing by half every fling half-life; a reset springs it
 * back to its home angles, critically damped so it never overshoots. Every
 * step is solved exactly for its dt, so the motion is the same at any frame
 * rate. Any entity can orbit this way: a camera, a light, a prop.
 *
 * Pure: steering arrives as turns already in radians, and the result is a
 * transformf_t the caller writes into its entity. Distance is held as its
 * logarithm, so a zoom scales it by the same ratio at any distance.
 */
#pragma once

#include <stdbool.h>

#include "util/math/math_const.h"
#include "util/math/transformf.h"
#include "util/math/vec3f.h"

/* The steepest pitch either way. Short of a quarter turn: the pose faces the
 * target with look_at against the world's up, which is undefined straight up
 * or down. */
#define ORBIT_MOTION_PITCH_LIMIT               (MATH_PI * 85.0F / 180.0F)

/* A fling's speed halves every this many seconds once steering lets go. */
#define ORBIT_MOTION_FLING_HALF_LIFE_S         0.35F

/* How fast the fling speed follows the steering while it is held: its speed
 * is averaged over about this half-life, so one jittery frame does not decide
 * the fling, and steering held still before letting go flings nothing. */
#define ORBIT_MOTION_DRAG_VELOCITY_HALF_LIFE_S 0.03F

/* The reset spring's time constant (1 / its angular frequency). */
#define ORBIT_MOTION_RESET_TIME_S              0.12F

/* A reset is over once every angle (radians) and log distance is this close
 * to home and moving slower than this per second. */
#define ORBIT_MOTION_RESET_SETTLED             1.0E-4F

/* For framing with a camera: each half-axis of the picture keeps this
 * fraction of itself clear around the framed sphere. */
#define ORBIT_MOTION_FRAME_PADDING             0.08F

typedef struct {
    float yaw;      /* radians about +y; 0 puts the transform on the target's +z side */
    float pitch;    /* radians; above 0 the transform is above the target, facing down */
    float log_dist; /* natural log of the transform's distance from the target */
} orbit_motion_state_t;

typedef struct {
    float pitch_min, pitch_max;       /* within +-ORBIT_MOTION_PITCH_LIMIT */
    float log_dist_min, log_dist_max; /* equal while there is no zoom */
} orbit_motion_limits_t;

typedef struct {
    vec3f_t target;
    orbit_motion_state_t at, home;
    orbit_motion_state_t velocity; /* each per second */
    orbit_motion_limits_t limits;
    float fling_half_life_s, drag_velocity_half_life_s, reset_time_s;
    bool resetting;
} orbit_motion_t;

/* One frame of steering. */
typedef struct {
    float yaw_turn, pitch_turn; /* radians to turn this frame */
    bool held;                  /* steering is held: the angles follow it and nothing flings */
    bool reset;                 /* start springing back to home */
    float zoom_turn;            /* change in log distance this frame; below 0 is closer */
} orbit_motion_input_t;

/* At home, still, with the default half-lives; `home` is clamped to `limits`. */
void orbit_motion_init(orbit_motion_t* orbit, vec3f_t target, orbit_motion_state_t home, orbit_motion_limits_t limits);

/* Advances dt_s seconds. Held steering cancels a reset. */
void orbit_motion_update(orbit_motion_t* orbit, const orbit_motion_input_t* input, float dt_s);

/* Where the transform stands, its +z facing the target and its +x level. */
transformf_t orbit_motion_pose(const orbit_motion_t* orbit);

/* For a camera that orbits: the distance at which a sphere of `radius`
 * centred on the view axis fits a lens of `half_fov_short_tan` (fitted to
 * the picture's shorter axis, as camera_t's is) with `padding` of each
 * half-axis left clear. The shorter axis binds, whichever way the picture
 * is held. */
float orbit_motion_fit_distance(float radius, float half_fov_short_tan, float padding);

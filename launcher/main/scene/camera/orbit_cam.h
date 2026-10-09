/*
 * orbit_cam: a turntable camera that circles a target. A finger turns it
 * about the world's up axis (yaw) and tilts it toward or away from the
 * target (pitch); let go mid-drag and it keeps turning, slowing by half
 * every fling half-life; a reset springs it back to its home angles,
 * critically damped so it never overshoots. Every step is solved exactly
 * for its dt, so the motion is the same at any frame rate.
 *
 * Pure: input arrives as turns already in radians (orbit_touch.h makes them
 * from a finger), and the result is a pose on util/math. Distance is held
 * as its logarithm so a zoom scales it evenly; zoom input is not wired yet.
 */
#pragma once

#include <stdbool.h>

#include "util/math/math_const.h"
#include "util/math/transformf.h"
#include "util/math/vec3f.h"

/* The steepest pitch either way. Short of a quarter turn so the camera never
 * looks straight down or up: camera_t needs a forward that is not vertical. */
#define ORBIT_PITCH_LIMIT               (MATH_PI * 85.0F / 180.0F)

/* A fling's speed halves every this many seconds once the finger lifts. */
#define ORBIT_FLING_HALF_LIFE_S         0.35F

/* How fast the fling speed follows the finger while it drags: the finger's
 * speed is averaged over about this half-life, so one jittery frame does not
 * decide the fling, and a finger held still before lifting flings nothing. */
#define ORBIT_DRAG_VELOCITY_HALF_LIFE_S 0.03F

/* The reset spring's time constant (1 / its angular frequency). */
#define ORBIT_RESET_TIME_S              0.12F

/* A reset is over once every angle (radians) and log distance is this close
 * to home and moving slower than this per second. */
#define ORBIT_RESET_SETTLED             1.0E-4F

/* Each half-axis of the picture keeps this fraction of itself clear around
 * the framed sphere. */
#define ORBIT_FRAME_PADDING             0.08F

typedef struct {
    float yaw;      /* radians about +y; 0 puts the eye on the target's +z side */
    float pitch;    /* radians; above 0 the eye is above the target, looking down */
    float log_dist; /* natural log of the eye's distance from the target */
} orbit_state_t;

typedef struct {
    float pitch_min, pitch_max;       /* within +-ORBIT_PITCH_LIMIT */
    float log_dist_min, log_dist_max; /* equal while there is no zoom */
} orbit_limits_t;

typedef struct {
    vec3f_t target;
    orbit_state_t at, home;
    orbit_state_t velocity; /* each per second */
    orbit_limits_t limits;
    float fling_half_life_s, drag_velocity_half_life_s, reset_time_s;
    bool resetting;
} orbit_cam_t;

/* One frame of steering. */
typedef struct {
    float yaw_turn, pitch_turn; /* radians the finger turned this frame */
    bool held;                  /* a finger is steering: the angles follow it and nothing flings */
    bool reset;                 /* start springing back to home */
} orbit_input_t;

/* At home, still, with the default half-lives; `home` is clamped to `limits`. */
void orbit_init(orbit_cam_t* cam, vec3f_t target, orbit_state_t home, orbit_limits_t limits);

/* Advances dt_s seconds. A held finger cancels a reset. */
void orbit_update(orbit_cam_t* cam, const orbit_input_t* input, float dt_s);

/* Where the eye stands, its +z looking at the target and its +x level. */
transformf_t orbit_pose(const orbit_cam_t* cam);

/* The distance at which a sphere of `radius` centred on the view axis fits a
 * lens of `half_fov_short_tan` (fitted to the picture's shorter axis, as
 * camera_t's is) with `padding` of each half-axis left clear. The shorter
 * axis binds, whichever way the picture is held. */
float orbit_fit_distance(float radius, float half_fov_short_tan, float padding);

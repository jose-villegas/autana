/*
 * lab_orbit: a lab scene's camera circling one subject, turned by one finger.
 *
 * It circles the subject's bounding sphere at the distance that fits it from
 * every angle (orbit_motion.h); a drag turns it, a double tap brings it home
 * (orbit_motion_touch.h), and it may also turn on its own at a set pace.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "input/input.h"
#include "input/mapping/orbit_motion_touch.h"
#include "input/touch/gesture.h"
#include "kinematics/orbit/orbit_motion.h"
#include "math/geometry/spheref.h"
#include "scene/entity/entity.h"

/* Where the orbit starts and how far it may go, relative to the subject. */
typedef struct {
    float home_yaw, home_pitch; /* radians */
    float pitch_min;            /* the lowest the eye goes, radians */
    /* How far a zoom reaches, as multiples of the distance that fits the
     * sphere: zoom_in below 1 brings it closer, zoom_out above 1 farther. */
    float zoom_in, zoom_out;
    /* Never closer than this many near planes outside the sphere, so the
     * near plane cannot cut into the subject. */
    float near_clearance;
} lab_orbit_frame_t;

typedef struct {
    orbit_motion_t motion;
    orbit_motion_touch_t touch;
    entity_t camera;
} lab_orbit_t;

/* Fits the orbit to `subject` as `camera`'s lens sees it, at home, and places
 * the camera. False when the camera has no lens. */
bool lab_orbit_fit(lab_orbit_t* orbit, entity_t camera, spheref_t subject, const lab_orbit_frame_t* frame);

/* The finger's turn this frame, with `home_edge` (shell_home_edge()) left to
 * the home swipe; then, when `turn_ms` is above 0, a full turn about the
 * subject every `turn_ms`. Places the camera. */
void lab_orbit_steer(lab_orbit_t* orbit, uint32_t dt_ms, const input_t* input, gesture_edge_t home_edge, int turn_ms);

/* Holds the orbit still at `yaw` and the home pitch, at the zoom-in limit when
 * `closest`, else at the home distance; places the camera. */
void lab_orbit_hold(lab_orbit_t* orbit, float yaw, bool closest);

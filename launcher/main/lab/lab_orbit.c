#include "lab/lab_orbit.h"

#include <math.h>

#include "display/display.h"
#include "gfx/gfx.h"
#include "math/scalar/math_const.h"
#include "math/scalar/units.h"
#include "scene/camera/camera.h"
#include "scene/transform/transform.h"

static void
place_camera(const lab_orbit_t* orbit) {
    transform_set_local(orbit->camera, orbit_motion_pose(&orbit->motion));
}

bool
lab_orbit_fit(lab_orbit_t* orbit, entity_t camera, spheref_t subject, const lab_orbit_frame_t* frame) {
    const lens_t* lens = camera_lens(camera);
    if (lens == NULL) {
        return false;
    }
    const float fit = orbit_motion_fit_distance(subject.radius, lens->half_fov_short_tan, ORBIT_MOTION_FRAME_PADDING);
    const float closest = fmaxf(fit * frame->zoom_in, subject.radius + (frame->near_clearance * lens->near_z));
    orbit_motion_init(&orbit->motion, subject.centre,
                      (orbit_motion_state_t){frame->home_yaw, frame->home_pitch, logf(fit)},
                      (orbit_motion_limits_t){frame->pitch_min, ORBIT_MOTION_PITCH_LIMIT, logf(closest),
                                              logf(fit * frame->zoom_out)});
    orbit_motion_touch_init(&orbit->touch);
    orbit->camera = camera;
    place_camera(orbit);
    return true;
}

void
lab_orbit_steer(lab_orbit_t* orbit, uint32_t dt_ms, const input_t* input, gesture_edge_t home_edge, int turn_ms) {
    const viewport_t panel = {GFX_WIDTH, GFX_HEIGHT, display_quarter_now()};
    const orbit_motion_input_t steering = orbit_motion_touch_step(&orbit->touch, input, dt_ms, panel, home_edge);
    orbit_motion_update(&orbit->motion, &steering, (float)dt_ms / (float)MATH_MS_PER_S);
    if (turn_ms > 0) {
        orbit->motion.at.yaw = fmodf(orbit->motion.at.yaw + (MATH_TAU * (float)dt_ms / (float)turn_ms), MATH_TAU);
    }
    place_camera(orbit);
}

void
lab_orbit_hold(lab_orbit_t* orbit, float yaw, bool closest) {
    orbit_motion_t* m = &orbit->motion;
    m->at = (orbit_motion_state_t){yaw, m->home.pitch, closest ? m->limits.log_dist_min : m->home.log_dist};
    m->velocity = (orbit_motion_state_t){0};
    m->resetting = false;
    place_camera(orbit);
}

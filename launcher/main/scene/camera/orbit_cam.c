#include "scene/camera/orbit_cam.h"

#include <math.h>

#pragma GCC diagnostic error "-Wdouble-promotion"

static float
clampf(float x, float lo, float hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}

/* What is left of anything halving every half_life after dt. */
static float
halved_over(float dt, float half_life) {
    return exp2f(-dt / half_life);
}

/* The angle from b to a, the short way round. */
static float
turn_between(float a, float b) {
    return remainderf(a - b, MATH_TAU);
}

static orbit_state_t
clamp_state(orbit_state_t s, const orbit_limits_t* limits) {
    s.pitch = clampf(s.pitch, limits->pitch_min, limits->pitch_max);
    s.log_dist = clampf(s.log_dist, limits->log_dist_min, limits->log_dist_max);
    return s;
}

void
orbit_init(orbit_cam_t* cam, vec3f_t target, orbit_state_t home, orbit_limits_t limits) {
    *cam = (orbit_cam_t){
        .target = target,
        .home = clamp_state(home, &limits),
        .limits = limits,
        .fling_half_life_s = ORBIT_FLING_HALF_LIFE_S,
        .drag_velocity_half_life_s = ORBIT_DRAG_VELOCITY_HALF_LIFE_S,
        .reset_time_s = ORBIT_RESET_TIME_S,
    };
    cam->at = cam->home;
}

/* The finger moves the angles itself; their velocity follows its speed, for
 * the fling when it lifts. */
static void
follow_finger(orbit_cam_t* cam, const orbit_input_t* input, float dt) {
    cam->at.yaw += input->yaw_turn;
    cam->at.pitch += input->pitch_turn;
    cam->velocity.log_dist = 0.0F;
    if (dt > 0.0F) {
        const float k = 1.0F - halved_over(dt, cam->drag_velocity_half_life_s);
        cam->velocity.yaw += (input->yaw_turn / dt - cam->velocity.yaw) * k;
        cam->velocity.pitch += (input->pitch_turn / dt - cam->velocity.pitch) * k;
    }
}

/* Velocity halving every half-life, and the exact distance it covers in dt. */
static void
fling(float* x, float* v, float dt, float half_life) {
    const float left = halved_over(dt, half_life);
    *x += *v * half_life / MATH_LN2 * (1.0F - left);
    *v *= left;
}

/* A critically damped spring toward 0 from offset *e at speed *v, exactly:
 * e(t) = (e + c t) exp(-w t) and v(t) = (v - w c t) exp(-w t), c = v + w e. */
static void
spring(float* e, float* v, float dt, float time_constant) {
    const float w = 1.0F / time_constant;
    const float left = expf(-w * dt);
    const float c = *v + (w * *e);
    *e = (*e + (c * dt)) * left;
    *v = (*v - (w * c * dt)) * left;
}

static bool
settled(float e, float v) {
    return fabsf(e) < ORBIT_RESET_SETTLED && fabsf(v) < ORBIT_RESET_SETTLED;
}

static void
spring_home(orbit_cam_t* cam, float dt) {
    float yaw = turn_between(cam->at.yaw, cam->home.yaw);
    float pitch = cam->at.pitch - cam->home.pitch;
    float log_dist = cam->at.log_dist - cam->home.log_dist;
    spring(&yaw, &cam->velocity.yaw, dt, cam->reset_time_s);
    spring(&pitch, &cam->velocity.pitch, dt, cam->reset_time_s);
    spring(&log_dist, &cam->velocity.log_dist, dt, cam->reset_time_s);
    cam->at = (orbit_state_t){cam->home.yaw + yaw, cam->home.pitch + pitch, cam->home.log_dist + log_dist};
    if (settled(yaw, cam->velocity.yaw) && settled(pitch, cam->velocity.pitch)
        && settled(log_dist, cam->velocity.log_dist)) {
        cam->at = cam->home;
        cam->velocity = (orbit_state_t){0};
        cam->resetting = false;
    }
}

/* Keeps yaw in one turn and stops whatever runs into a limit. */
static void
hold_to_limits(orbit_cam_t* cam) {
    cam->at.yaw = remainderf(cam->at.yaw, MATH_TAU);
    const orbit_state_t held = clamp_state(cam->at, &cam->limits);
    if (held.pitch != cam->at.pitch) {
        cam->velocity.pitch = 0.0F;
    }
    if (held.log_dist != cam->at.log_dist) {
        cam->velocity.log_dist = 0.0F;
    }
    cam->at = held;
}

void
orbit_update(orbit_cam_t* cam, const orbit_input_t* input, float dt_s) {
    if (input->held) {
        cam->resetting = false;
        follow_finger(cam, input, dt_s);
    } else {
        cam->resetting = cam->resetting || input->reset;
        if (cam->resetting) {
            spring_home(cam, dt_s);
        } else {
            fling(&cam->at.yaw, &cam->velocity.yaw, dt_s, cam->fling_half_life_s);
            fling(&cam->at.pitch, &cam->velocity.pitch, dt_s, cam->fling_half_life_s);
            fling(&cam->at.log_dist, &cam->velocity.log_dist, dt_s, cam->fling_half_life_s);
        }
    }
    hold_to_limits(cam);
}

transformf_t
orbit_pose(const orbit_cam_t* cam) {
    const float d = expf(cam->at.log_dist);
    const float level = cosf(cam->at.pitch);
    const vec3f_t out = {level * sinf(cam->at.yaw), sinf(cam->at.pitch), level * cosf(cam->at.yaw)};
    transformf_t pose = TRANSFORMF_IDENTITY;
    transformf_set_position(&pose, vec3f_add(cam->target, vec3f_scale(out, d)));
    transformf_look_at(&pose, cam->target, (vec3f_t){0.0F, 1.0F, 0.0F});
    return pose;
}

/* A sphere of radius r at distance d spans the cone of half-angle asin(r/d),
 * which meets the unit-depth image plane in a circle of radius tan of that.
 * Setting it to the padded half-width t: d = r / sin(atan t) = r sqrt(1 + t^2) / t. */
float
orbit_fit_distance(float radius, float half_fov_short_tan, float padding) {
    const float t = half_fov_short_tan * (1.0F - padding);
    return radius * sqrtf(1.0F + (t * t)) / t;
}

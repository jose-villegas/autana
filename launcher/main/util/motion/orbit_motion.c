#include "util/motion/orbit_motion.h"

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

static orbit_motion_state_t
clamp_state(orbit_motion_state_t s, const orbit_motion_limits_t* limits) {
    s.pitch = clampf(s.pitch, limits->pitch_min, limits->pitch_max);
    s.log_dist = clampf(s.log_dist, limits->log_dist_min, limits->log_dist_max);
    return s;
}

void
orbit_motion_init(orbit_motion_t* orbit, vec3f_t target, orbit_motion_state_t home, orbit_motion_limits_t limits) {
    *orbit = (orbit_motion_t){
        .target = target,
        .home = clamp_state(home, &limits),
        .limits = limits,
        .fling_half_life_s = ORBIT_MOTION_FLING_HALF_LIFE_S,
        .drag_velocity_half_life_s = ORBIT_MOTION_DRAG_VELOCITY_HALF_LIFE_S,
        .reset_time_s = ORBIT_MOTION_RESET_TIME_S,
    };
    orbit->at = orbit->home;
}

/* Held steering moves the angles itself; their velocity follows its speed,
 * for the fling when it lets go. */
static void
follow_steering(orbit_motion_t* orbit, const orbit_motion_input_t* input, float dt) {
    orbit->at.yaw += input->yaw_turn;
    orbit->at.pitch += input->pitch_turn;
    orbit->at.log_dist += input->zoom_turn;
    if (dt > 0.0F) {
        const float k = 1.0F - halved_over(dt, orbit->drag_velocity_half_life_s);
        orbit->velocity.yaw += (input->yaw_turn / dt - orbit->velocity.yaw) * k;
        orbit->velocity.pitch += (input->pitch_turn / dt - orbit->velocity.pitch) * k;
        orbit->velocity.log_dist += (input->zoom_turn / dt - orbit->velocity.log_dist) * k;
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
    return fabsf(e) < ORBIT_MOTION_RESET_SETTLED && fabsf(v) < ORBIT_MOTION_RESET_SETTLED;
}

static void
spring_home(orbit_motion_t* orbit, float dt) {
    float yaw = turn_between(orbit->at.yaw, orbit->home.yaw);
    float pitch = orbit->at.pitch - orbit->home.pitch;
    float log_dist = orbit->at.log_dist - orbit->home.log_dist;
    spring(&yaw, &orbit->velocity.yaw, dt, orbit->reset_time_s);
    spring(&pitch, &orbit->velocity.pitch, dt, orbit->reset_time_s);
    spring(&log_dist, &orbit->velocity.log_dist, dt, orbit->reset_time_s);
    orbit->at =
        (orbit_motion_state_t){orbit->home.yaw + yaw, orbit->home.pitch + pitch, orbit->home.log_dist + log_dist};
    if (settled(yaw, orbit->velocity.yaw) && settled(pitch, orbit->velocity.pitch)
        && settled(log_dist, orbit->velocity.log_dist)) {
        orbit->at = orbit->home;
        orbit->velocity = (orbit_motion_state_t){0};
        orbit->resetting = false;
    }
}

/* Keeps yaw in one turn and stops whatever runs into a limit. */
static void
hold_to_limits(orbit_motion_t* orbit) {
    orbit->at.yaw = remainderf(orbit->at.yaw, MATH_TAU);
    const orbit_motion_state_t held = clamp_state(orbit->at, &orbit->limits);
    if (held.pitch != orbit->at.pitch) {
        orbit->velocity.pitch = 0.0F;
    }
    if (held.log_dist != orbit->at.log_dist) {
        orbit->velocity.log_dist = 0.0F;
    }
    orbit->at = held;
}

void
orbit_motion_update(orbit_motion_t* orbit, const orbit_motion_input_t* input, float dt_s) {
    if (input->held) {
        orbit->resetting = false;
        follow_steering(orbit, input, dt_s);
    } else {
        orbit->resetting = orbit->resetting || input->reset;
        if (orbit->resetting) {
            spring_home(orbit, dt_s);
        } else {
            fling(&orbit->at.yaw, &orbit->velocity.yaw, dt_s, orbit->fling_half_life_s);
            fling(&orbit->at.pitch, &orbit->velocity.pitch, dt_s, orbit->fling_half_life_s);
            fling(&orbit->at.log_dist, &orbit->velocity.log_dist, dt_s, orbit->fling_half_life_s);
        }
    }
    hold_to_limits(orbit);
}

/* Stands at the target's +z side facing it, tilts by pitch about its own
 * right, then turns by yaw about the world's up: rigid turns about the
 * target, so it faces the target all the way and its right stays level. */
transformf_t
orbit_motion_pose(const orbit_motion_t* orbit) {
    const vec3f_t up = {0.0F, 1.0F, 0.0F};
    transformf_t pose = TRANSFORMF_IDENTITY;
    transformf_set_position(&pose, vec3f_add(orbit->target, (vec3f_t){0.0F, 0.0F, expf(orbit->at.log_dist)}));
    transformf_look_at(&pose, orbit->target, up);
    transformf_rotate_around(&pose, orbit->target, quatf_rotate(pose.rotation, (vec3f_t){1.0F, 0.0F, 0.0F}),
                             orbit->at.pitch);
    transformf_rotate_around(&pose, orbit->target, up, orbit->at.yaw);
    return pose;
}

/* A sphere of radius r at distance d spans the cone of half-angle asin(r/d),
 * which meets the unit-depth image plane in a circle of radius tan of that.
 * Setting it to the padded half-width t: d = r / sin(atan t) = r sqrt(1 + t^2) / t. */
float
orbit_motion_fit_distance(float radius, float half_fov_short_tan, float padding) {
    const float t = half_fov_short_tan * (1.0F - padding);
    return radius * sqrtf(1.0F + (t * t)) / t;
}

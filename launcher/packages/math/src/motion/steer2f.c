/* steer2f: Reynolds's arrive, separation, unaligned collision avoidance and
 * containment, and a limited step; see steer2f.h. */
#include "math/motion/steer2f.h"

#include <math.h>

#include "math/scalar/math_const.h"

vec2f_t
steer2f_arrive(const steer2f_agent_t* a, vec2f_t target, float speed, const steer2f_limits_t* l) {
    const vec2f_t to = vec2f_sub(target, a->position);
    const float distance = vec2f_length(to);
    vec2f_t desired = vec2f_zero();
    if (distance > 0.0F) {
        const float slowed = speed * fminf(1.0F, distance / l->arrive_radius);
        desired = vec2f_scale(to, slowed / distance);
    }
    return vec2f_clamp_length(vec2f_sub(desired, a->velocity), l->max_accel);
}

vec2f_t
steer2f_separation(const steer2f_agent_t* agents, int count, int self, const steer2f_limits_t* l) {
    vec2f_t push = vec2f_zero();
    for (int j = 0; j < count; j++) {
        if (j == self) {
            continue;
        }
        const vec2f_t away = vec2f_sub(agents[self].position, agents[j].position);
        const float squared = vec2f_dot(away, away);
        if (squared >= l->separation_radius * l->separation_radius) {
            continue;
        }
        const float distance = sqrtf(squared);
        /* Two agents on one spot part along x, the lower index to -x, so the push stays determined. */
        const vec2f_t direction =
            distance > 0.0F ? vec2f_scale(away, 1.0F / distance) : (vec2f_t){self < j ? -1.0F : 1.0F, 0.0F};
        push = vec2f_add(push, vec2f_scale(direction, 1.0F - (distance / l->separation_radius)));
    }
    return vec2f_clamp_length(vec2f_scale(push, l->max_accel), l->max_accel);
}

vec2f_t
steer2f_avoid(const steer2f_agent_t* agents, int count, int self, const steer2f_limits_t* l) {
    const steer2f_agent_t* me = &agents[self];
    vec2f_t push = vec2f_zero();
    for (int j = 0; j < count; j++) {
        if (j == self) {
            continue;
        }
        const vec2f_t dp = vec2f_sub(agents[j].position, me->position);
        const vec2f_t dv = vec2f_sub(agents[j].velocity, me->velocity);
        const float closing = vec2f_dot(dv, dv);
        if (closing <= 0.0F) {
            continue;
        }
        const float ahead = -vec2f_dot(dp, dv); /* t = ahead / closing */
        if (ahead <= 0.0F || ahead > l->horizon_s * closing) {
            continue;
        }
        const float t = ahead / closing;
        /* Our offset from the neighbour at the time the two paths come closest. */
        const vec2f_t miss = vec2f_scale(vec2f_add(dp, vec2f_scale(dv, t)), -1.0F);
        const float distance = vec2f_length(miss);
        if (distance >= l->separation_radius) {
            continue;
        }
        /* Dead on, part across the closing line: dv flips sign between the two,
         * so each turns to its own left and they part opposite ways. */
        const vec2f_t across = vec2f_scale((vec2f_t){-dv.y, dv.x}, 1.0F / sqrtf(closing));
        const vec2f_t direction = distance > 0.0F ? vec2f_scale(miss, 1.0F / distance) : across;
        const float weight = (1.0F - (distance / l->separation_radius)) * (1.0F - (t / l->horizon_s));
        push = vec2f_add(push, vec2f_scale(direction, weight));
    }
    return vec2f_clamp_length(vec2f_scale(push, l->max_accel), l->max_accel);
}

vec2f_t
steer2f_contain(const steer2f_agent_t* a, vec2f_t centre, float radius, const steer2f_limits_t* l) {
    const vec2f_t in = vec2f_sub(centre, a->position);
    const float squared = vec2f_dot(in, in);
    if (squared <= radius * radius) {
        return vec2f_zero();
    }
    const float distance = sqrtf(squared);
    const float past = distance - radius;
    return vec2f_scale(in, l->max_accel * fminf(1.0F, past / l->arrive_radius) / distance);
}

/* `angle` in (-3 pi, 3 pi) brought into (-pi, pi]. */
static float
wrap_pi(float angle) {
    if (angle > MATH_PI) {
        return angle - MATH_TAU;
    }
    return angle <= -MATH_PI ? angle + MATH_TAU : angle;
}

void
steer2f_step(steer2f_agent_t* a, vec2f_t accel, float dt, const steer2f_limits_t* l) {
    a->velocity = vec2f_clamp_length(vec2f_add(a->velocity, vec2f_scale(vec2f_clamp_length(accel, l->max_accel), dt)),
                                     l->max_speed);
    a->position = vec2f_add(a->position, vec2f_scale(a->velocity, dt));
    if (vec2f_length(a->velocity) < l->heading_speed) {
        return;
    }
    const float turn = wrap_pi(atan2f(a->velocity.y, a->velocity.x) - a->heading);
    const float most = l->turn_rate * dt;
    a->heading = wrap_pi(a->heading + fmaxf(-most, fminf(most, turn)));
}

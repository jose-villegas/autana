/* steer2f: Reynolds's arrive and separation, and a limited step; see steer2f.h. */
#include "math/motion/steer2f.h"

#include <math.h>

#include "math/scalar/math_const.h"

static float
length(vec2f_t v) {
    return sqrtf(vec2f_dot(v, v));
}

/* v cut to at most `most` long, its direction kept. */
static vec2f_t
truncate(vec2f_t v, float most) {
    const float len = length(v);
    return len > most ? vec2f_scale(v, most / len) : v;
}

vec2f_t
steer2f_arrive(const steer2f_agent_t* a, vec2f_t target, float speed, const steer2f_limits_t* l) {
    const vec2f_t to = vec2f_sub(target, a->position);
    const float distance = length(to);
    vec2f_t desired = vec2f_zero();
    if (distance > 0.0F) {
        const float slowed = speed * fminf(1.0F, distance / l->arrive_radius);
        desired = vec2f_scale(to, slowed / distance);
    }
    return truncate(vec2f_sub(desired, a->velocity), l->max_accel);
}

vec2f_t
steer2f_separation(const steer2f_agent_t* agents, int count, int self, const steer2f_limits_t* l) {
    vec2f_t push = vec2f_zero();
    for (int j = 0; j < count; j++) {
        if (j == self) {
            continue;
        }
        const vec2f_t away = vec2f_sub(agents[self].position, agents[j].position);
        const float distance = length(away);
        if (distance >= l->separation_radius) {
            continue;
        }
        /* Two agents on one spot part along x, the lower index to -x, so the push stays determined. */
        const vec2f_t direction =
            distance > 0.0F ? vec2f_scale(away, 1.0F / distance) : (vec2f_t){self < j ? -1.0F : 1.0F, 0.0F};
        push = vec2f_add(push, vec2f_scale(direction, 1.0F - (distance / l->separation_radius)));
    }
    return truncate(vec2f_scale(push, l->max_accel), l->max_accel);
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
        const float t = -vec2f_dot(dp, dv) / closing;
        if (t <= 0.0F || t > l->horizon_s) {
            continue;
        }
        /* Our offset from the neighbour at the time the two paths come closest. */
        const vec2f_t miss = vec2f_scale(vec2f_add(dp, vec2f_scale(dv, t)), -1.0F);
        const float distance = length(miss);
        if (distance >= l->separation_radius) {
            continue;
        }
        /* Dead on, part across the closing line, the lower index to its left. */
        const vec2f_t across = vec2f_scale((vec2f_t){-dv.y, dv.x}, (self < j ? 1.0F : -1.0F) / sqrtf(closing));
        const vec2f_t direction = distance > 0.0F ? vec2f_scale(miss, 1.0F / distance) : across;
        const float weight = (1.0F - (distance / l->separation_radius)) * (1.0F - (t / l->horizon_s));
        push = vec2f_add(push, vec2f_scale(direction, weight));
    }
    return truncate(vec2f_scale(push, l->max_accel), l->max_accel);
}

vec2f_t
steer2f_contain(const steer2f_agent_t* a, vec2f_t centre, float radius, const steer2f_limits_t* l) {
    const vec2f_t in = vec2f_sub(centre, a->position);
    const float distance = length(in);
    const float past = distance - radius;
    if (past <= 0.0F) {
        return vec2f_zero();
    }
    return vec2f_scale(in, l->max_accel * fminf(1.0F, past / l->arrive_radius) / distance);
}

void
steer2f_step(steer2f_agent_t* a, vec2f_t accel, float dt, const steer2f_limits_t* l) {
    a->velocity = truncate(vec2f_add(a->velocity, vec2f_scale(truncate(accel, l->max_accel), dt)), l->max_speed);
    a->position = vec2f_add(a->position, vec2f_scale(a->velocity, dt));
    if (length(a->velocity) < l->heading_speed) {
        return;
    }
    const float turn = remainderf(atan2f(a->velocity.y, a->velocity.x) - a->heading, MATH_TAU);
    const float most = l->turn_rate * dt;
    a->heading = remainderf(a->heading + fmaxf(-most, fminf(most, turn)), MATH_TAU);
}

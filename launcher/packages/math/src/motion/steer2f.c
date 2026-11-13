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

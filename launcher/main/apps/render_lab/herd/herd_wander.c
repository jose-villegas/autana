/* herd_wander: see herd_wander.h. */
#include "apps/render_lab/herd/herd_wander.h"

#include <assert.h>
#include <math.h>

#include "math/scalar/math_const.h"
#include "math/scalar/units.h"

/* A moving leg ends this close to its target; inside arrive_radius, so the copy is slowing. */
#define HERD_WANDER_ARRIVED_M    0.3F
/* Draws a far-enough target at most this many times, then keeps the last. */
#define HERD_WANDER_TARGET_TRIES 8

quatf_t
herd_wander_facing(float heading) {
    return quatf_from_axis_angle(vec3f_unit_y(), -(heading + (MATH_PI / 2.0F)));
}

float
herd_wander_heading(quatf_t rotation) {
    const vec3f_t forward = quatf_rotate(rotation, vec3f_scale(vec3f_unit_z(), -1.0F));
    return atan2f(forward.z, forward.x);
}

/* A point uniform over the area's disc. */
static vec2f_t
area_point(herd_wander_t* w) {
    const float r = w->config->area_radius * sqrtf(rng_unit(&w->rng));
    const float angle = MATH_TAU * rng_unit(&w->rng);
    return (vec2f_t){r * cosf(angle), r * sinf(angle)};
}

static void
draw_leg(herd_wander_t* w, herd_walker_t* walker) {
    const herd_wander_config_t* c = w->config;
    walker->leg_speed = c->gait->steps[rng_below(&w->rng, c->gait->count)].ground_mps;
    if (walker->leg_speed <= 0.0F) {
        walker->target = walker->agent.position;
        walker->graze_ms = c->graze_min_ms + (uint32_t)rng_below(&w->rng, (int)(c->graze_max_ms - c->graze_min_ms) + 1);
        return;
    }
    walker->graze_ms = 0;
    for (int i = 0; i < HERD_WANDER_TARGET_TRIES; i++) {
        walker->target = area_point(w);
        if (vec2f_length(vec2f_sub(walker->target, walker->agent.position)) >= c->leg_min_m) {
            break;
        }
    }
}

static void
restart(herd_wander_t* w) {
    rng_seed(&w->rng, w->seed);
    w->t_ms = 0;
    w->carry_ms = 0;
    for (int i = 0; i < w->count; i++) {
        herd_walker_t* walker = &w->walker[i];
        walker->agent = w->start[i];
        walker->gait = gait_pick(w->config->gait, (gait_pick_t){-1, 1.0F}, 0.0F);
        walker->clip_s = 0.0F;
        draw_leg(w, walker);
    }
}

void
herd_wander_start(herd_wander_t* w, const herd_wander_config_t* config, uint32_t seed, const steer2f_agent_t* start,
                  int count) {
    assert(count >= 0 && count <= HERD_WANDER_MAX);
    w->config = config;
    w->seed = seed;
    w->count = count;
    for (int i = 0; i < count; i++) {
        w->start[i] = start[i];
    }
    restart(w);
}

/* One step: every copy's steer from where all of them stood, then every copy moves. */
static void
step(herd_wander_t* w) {
    const herd_wander_config_t* c = w->config;
    const float dt = (float)HERD_WANDER_STEP_MS / (float)MATH_MS_PER_S;
    steer2f_agent_t agents[HERD_WANDER_MAX];
    for (int i = 0; i < w->count; i++) {
        agents[i] = w->walker[i].agent;
    }
    for (int i = 0; i < w->count; i++) {
        herd_walker_t* walker = &w->walker[i];
        const vec2f_t apart = vec2f_add(steer2f_separation(agents, w->count, i, &c->limits),
                                        steer2f_avoid(agents, w->count, i, &c->limits));
        const vec2f_t keep = vec2f_add(apart, steer2f_contain(&agents[i], vec2f_zero(), c->area_radius, &c->limits));
        /* Keeping apart and inside comes first; arrive has what the acceleration limit leaves. */
        const float left = fmaxf(0.0F, c->limits.max_accel - vec2f_length(keep));
        const vec2f_t go = steer2f_arrive(&agents[i], walker->target, walker->leg_speed, &c->limits);
        const vec2f_t accel = vec2f_add(keep, vec2f_clamp_length(go, left));
        steer2f_step(&walker->agent, accel, dt, &c->limits);
        const gait_pick_t pick = gait_pick(c->gait, walker->gait, vec2f_length(walker->agent.velocity));
        walker->clip_s = pick.step == walker->gait.step ? walker->clip_s + (pick.rate * dt) : 0.0F;
        walker->gait = pick;
        if (walker->leg_speed <= 0.0F) {
            walker->graze_ms = walker->graze_ms > HERD_WANDER_STEP_MS ? walker->graze_ms - HERD_WANDER_STEP_MS : 0;
            if (walker->graze_ms == 0) {
                draw_leg(w, walker);
            }
        } else if (vec2f_length(vec2f_sub(walker->target, walker->agent.position)) < HERD_WANDER_ARRIVED_M) {
            draw_leg(w, walker);
        }
    }
}

void
herd_wander_advance(herd_wander_t* w, uint32_t dt_ms) {
    w->carry_ms += dt_ms;
    while (w->carry_ms >= HERD_WANDER_STEP_MS) {
        step(w);
        w->t_ms += HERD_WANDER_STEP_MS;
        w->carry_ms -= HERD_WANDER_STEP_MS;
    }
}

void
herd_wander_seek(herd_wander_t* w, uint32_t t_ms) {
    restart(w);
    herd_wander_advance(w, t_ms);
}

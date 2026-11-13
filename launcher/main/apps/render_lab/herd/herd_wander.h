/*
 * herd_wander: the herd's copies walking the meadow. Each copy takes legs to
 * seeded points inside a circle, at a seeded speed or grazing in place, keeps
 * apart from the others (steer2f) and picks its clip and rate from its ground
 * speed (gait). Fixed steps from a seed and a start, so any time is reached
 * again exactly: seek restarts and steps to it.
 */
#pragma once

#include <stdint.h>

#include "apps/render_lab/locomotion/gait.h"
#include "math/motion/steer2f.h"
#include "math/scalar/rng.h"

/* The simulation's step; time between steps is carried. */
#define HERD_WANDER_STEP_MS 20
/* The most copies one herd walks: the pool's size. */
#define HERD_WANDER_MAX     8

typedef struct {
    steer2f_limits_t limits;
    float area_radius;       /* metres round the origin a target is drawn in */
    float leg_min_m;         /* a new target lies at least this far from the copy */
    const float* leg_speeds; /* metres per second a leg is drawn at; 0 grazes in place */
    int leg_count;
    uint32_t graze_min_ms, graze_max_ms; /* how long a grazing leg lasts */
    const gait_t* gait;
} herd_wander_config_t;

typedef struct {
    steer2f_agent_t agent;
    vec2f_t target;
    float leg_speed;   /* this leg's; 0 while grazing */
    uint32_t graze_ms; /* grazing time left */
    gait_pick_t gait;
    float clip_s; /* seconds into the gait's clip at playback 1: the rate integrated since its step began */
} herd_walker_t;

typedef struct {
    const herd_wander_config_t* config;
    uint32_t seed;
    int count;
    steer2f_agent_t start[HERD_WANDER_MAX];
    herd_walker_t walker[HERD_WANDER_MAX];
    rng_t rng;
    uint32_t t_ms;     /* time stepped so far */
    uint32_t carry_ms; /* time given but short of a step */
} herd_wander_t;

/* Puts `count` copies (at most HERD_WANDER_MAX) at `start`, at time 0. */
void herd_wander_start(herd_wander_t* w, const herd_wander_config_t* config, uint32_t seed,
                       const steer2f_agent_t* start, int count);
/* Moves time on by dt_ms, in whole steps. */
void herd_wander_advance(herd_wander_t* w, uint32_t dt_ms);
/* The herd at t_ms from its start: as if advanced there from time 0. */
void herd_wander_seek(herd_wander_t* w, uint32_t t_ms);

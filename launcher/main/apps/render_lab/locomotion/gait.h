/*
 * gait: which clip a walking character plays at a ground speed, and how fast,
 * so its feet keep pace with the ground. Steps are clips by the ground speed
 * each shows at playback 1, slowest first (a still clip at 0). A still step
 * holds only up to `stop_mps`: any faster speed takes the slowest moving step,
 * at a rate the clamp slows, so a slow walker walks instead of sliding. Between
 * two moving steps a speed takes the nearer one, and a step is held while the
 * speed stays within `band` of its range, so it does not flip clips.
 */
#pragma once

#include <stdint.h>

#include "scene/scene.h"

typedef struct {
    int clip;         /* the animator's clip index */
    float ground_mps; /* metres per second the clip shows at playback 1; 0 for a still clip */
} gait_step_t;

typedef struct {
    const gait_step_t* steps; /* slowest first */
    int count;
    float rate_min, rate_max; /* playback rates a moving step keeps to */
    float band;               /* metres per second a held moving step reaches past its range */
    float stop_mps;           /* the fastest speed a still step plays at */
    uint32_t blend_ms;        /* the cross-fade into a new step */
} gait_t;

typedef struct {
    int step;   /* index into gait_t.steps */
    float rate; /* playback rate */
} gait_pick_t;

/* Orders `count` steps slowest first, as gait_t takes them. */
void gait_sort(gait_step_t* steps, int count);

/* The step for `speed` (metres per second), holding `current` inside its band,
 * and the rate that matches the step's ground speed to `speed`, clamped;
 * a still step plays at 1. current.step < 0 picks without holding. */
gait_pick_t gait_pick(const gait_t* g, gait_pick_t current, float speed);

/* Puts `now` on the entity's animator: a cross-fade into its clip when the
 * step changed from `was`, and its rate always. */
void gait_apply(entity_t entity, const gait_t* g, gait_pick_t was, gait_pick_t now);

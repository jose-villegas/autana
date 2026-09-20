/*
 * rt_pixel_budget - a progressive tracer's per-frame pixel allowance: grows
 * by one step while the previous frame finished inside its target, and
 * gives back exactly one step when it didn't. Shrinking by the amount it
 * grows by is what keeps a single over-budget frame cheap: halving costs
 * one frame to give up what the linear climb then needs hundreds to regain.
 */
#pragma once

#include <stdint.h>

typedef struct {
    int value;
    int step;
    int min;
    int max;
    uint32_t target_frame_ms;
} rt_pixel_budget_t;

static inline rt_pixel_budget_t
rt_pixel_budget_init(int step, int min, int max, uint32_t target_frame_ms) {
    return (rt_pixel_budget_t){.value = step, .step = step, .min = min, .max = max, .target_frame_ms = target_frame_ms};
}

static inline void
rt_pixel_budget_adapt(rt_pixel_budget_t* budget, uint32_t last_dt_ms) {
    if (last_dt_ms > budget->target_frame_ms) {
        budget->value -= budget->step;
        if (budget->value < budget->min) {
            budget->value = budget->min;
        }
    } else if (budget->value < budget->max) {
        budget->value += budget->step;
    }
}

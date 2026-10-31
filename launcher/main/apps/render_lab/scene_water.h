/* scene_water's counts and flip3's stage times, for the water perf suite. */
#pragma once

#include <stdint.h>

#include "physics/fluid/flip3.h"

/* Summed over the updates since the last take. */
typedef struct {
    uint32_t updates;
    uint32_t substeps;
    uint32_t remeshed; /* updates whose two-core mesh overflowed a half and was meshed again whole */
    int64_t update_us;
    int64_t flip3_us[FLIP3_STAGE_COUNT]; /* in a development build; the stages' own times are frame_cost's */
    int64_t triangles;
    int particles;         /* now */
    float peak_speed_mm_s; /* the fastest particle at the last update */
} scene_water_stats_t;

/* Returns the sums so far and starts new ones. */
scene_water_stats_t scene_water_take_stats(void);

/* The scripted tour (`render water tour`): its length, and the phase it is
 * in now, one of a few names that each cover a stretch of it. */
uint32_t scene_water_tour_length_ms(void);
const char* scene_water_tour_phase(void);

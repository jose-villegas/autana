/* scene_water's per-stage times, for the water perf suite and the status line. */
#pragma once

#include <stdint.h>

#include "physics/fluid/flip3.h"

/* The update's stages, in the order they run, with the names its log gives
 * them: the substeps, then the surface (field, light through it, mesh,
 * shade, dynamic mesh build). */
#define SCENE_WATER_STAGES(X)                                                                                          \
    X(STEP, "step") X(FIELD, "field") X(THROUGH, "through") X(NETS, "nets") X(SHADE, "shade") X(BUILD, "build")

#define SCENE_WATER_STAGE_ENUM(id, name) SCENE_WATER_STAGE_##id,

typedef enum { SCENE_WATER_STAGES(SCENE_WATER_STAGE_ENUM) SCENE_WATER_STAGE_COUNT } scene_water_stage_t;

extern const char* const scene_water_stage_names[SCENE_WATER_STAGE_COUNT];

/* Summed over the updates since the last take. */
typedef struct {
    uint32_t updates;
    uint32_t substeps;
    uint32_t rested; /* updates that skipped the step and the surface */
    int64_t update_us;
    int64_t stage_us[SCENE_WATER_STAGE_COUNT];
    int64_t flip3_us[FLIP3_STAGE_COUNT];
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

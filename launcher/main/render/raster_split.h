/* raster_split: caller-owned core-1 shares for the raster's two-core stages (core/job.h steers them). */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "build/build_variant.h"
#include "core/job.h"

/* Bounds the split state size; instances past this cap split evenly. */
#define RASTER_SPLIT_INSTANCES 16

/* Rotate the probe phase so even short lists are sampled. */
#define R3D_HIDDEN_PROBE_EVERY 32U

typedef struct {
    float hidden_rate, scan_rate;
    bool valid;
    unsigned probe_phase;
} r3d_hidden_rates_t;

typedef struct {
    uint64_t box_pixels, pixels_read;
    uint32_t tested, skipped, candidates;
} r3d_hidden_tally_t;

#if BUILD_VARIANT_DEVELOPMENT_OR_HOST
typedef enum {
    R3D_HIDDEN_VISIBLE = 1,
    R3D_HIDDEN_SKIPPED,
    R3D_HIDDEN_BYPASSED,
} r3d_hidden_outcome_t;

typedef void (*r3d_hidden_report_t)(void* state, int entry, int cluster, unsigned slice, r3d_hidden_outcome_t outcome);
#endif

/* Each stage's offset from an even share, in permille; per instance where a
 * stage runs per instance, and the view's previous hidden-test rates. */
typedef struct {
    int16_t top_offset[RASTER_SPLIT_INSTANCES]; /* draw: core 1 takes the top rows */
    int16_t transform_offset[RASTER_SPLIT_INSTANCES];
    int16_t upscale_offset;
    int16_t cull_offset; /* core 1 culls the last instances */
    r3d_hidden_rates_t hidden;
} raster_split_t;

void raster_split_forget(raster_split_t* split);
void raster_split_fold_hidden(raster_split_t* split, const r3d_hidden_tally_t* a, const r3d_hidden_tally_t* b);

/* raster_split: caller-owned core-1 shares for the raster's two-core stages (core/job.h steers them). */
#pragma once

#include <stdint.h>

#include "core/job.h"

/* Bounds the split state size; instances past this cap split evenly. */
#define RASTER_SPLIT_INSTANCES 16

/* Each stage's offset from an even share, in permille; per instance where a
 * stage runs per instance. Forget on a cut; NULL state keeps every split even. */
typedef struct {
    int16_t top_offset[RASTER_SPLIT_INSTANCES]; /* draw: core 1 takes the top rows */
    int16_t transform_offset[RASTER_SPLIT_INSTANCES];
    int16_t resolve_offset;
    int16_t upscale_offset;
} raster_split_t;

void raster_split_forget(raster_split_t* split);

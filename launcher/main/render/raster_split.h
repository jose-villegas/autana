/* raster_split: caller-owned, damped feedback for each instance's top draw share. */
#pragma once

#include <stdint.h>

/* Bounds the split state size; instances past this cap draw with an even share. */
#define RASTER_SPLIT_INSTANCES 16
#define RASTER_SPLIT_PERMILLE  1000
#define RASTER_SPLIT_EVEN      (RASTER_SPLIT_PERMILLE / 2)
/* Keeps either core from being starved by one noisy frame. */
#define RASTER_SPLIT_MIN       150
#define RASTER_SPLIT_MAX       (RASTER_SPLIT_PERMILLE - RASTER_SPLIT_MIN)

/* Caller-owned shares of draw weight assigned to the top slice, by instance.
 * Offsets from even are in permille. Forget on a cut; NULL state keeps every draw even.
 * Instances beyond the state capacity also draw with an even share. */
typedef struct {
    int16_t top_offset[RASTER_SPLIT_INSTANCES];
} raster_split_t;

void raster_split_forget(raster_split_t* split);
/* Half-damped balanced share from positive slice times.
 * Nonpositive times leave the share unchanged. */
int raster_split_update(int top_permille, int64_t top_us, int64_t bottom_us);

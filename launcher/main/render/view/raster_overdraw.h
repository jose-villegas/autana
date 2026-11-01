/*
 * raster_overdraw: how many times the fill reached each pixel this frame, as
 * a development view. A pixel is tested once for every span of a triangle
 * that covers it, and written when that triangle won its depth test; work
 * culled before the fill (clusters, hidden tiles) never reaches a pixel.
 * Painted as a heat ramp of the tested layers on a fixed scale, so frames
 * compare; the frame's totals are kept for a caller to report.
 *
 * Removable as its files plus its view table row.
 */
#pragma once

#include <inttypes.h>
#include <stdint.h>

#include "render/raster_attachment.h"

/* The ramp's steps: one to this many layers, and more painted as the last. */
#define RASTER_OVERDRAW_RAMP_LAYERS 5

/* A pixel's layers, each saturating at its type's maximum. */
typedef struct {
    uint8_t tested, written;
} raster_overdraw_px_t;

/* One frame, over the pixels tested at least once: their count, the layers
 * summed over them, and the most at any one. */
typedef struct {
    int32_t pixels;
    int64_t tested, written;
    int tested_max, written_max;
} raster_overdraw_frame_t;

typedef struct {
    raster_overdraw_frame_t frame; /* the last shown picture's */
} raster_overdraw_t;

/* The attachment, storing its state in `state`. */
raster_attachment_t raster_overdraw_view(void* state);

/* A frame's totals as one line, its fields in raster_overdraw_frame_t's order,
 * for a tool to read. */
#define RASTER_OVERDRAW_LINE                                                                                           \
    "overdraw: pixels=%" PRId32 " tested=%" PRId64 " written=%" PRId64 " tested_max=%d written_max=%d"

/* The last shown picture's totals. */
raster_overdraw_frame_t raster_overdraw_frame(const raster_overdraw_t* state);

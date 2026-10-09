/*
 * raster_motion: previous-position offsets from camera depth and previous
 * instance placements, in current-picture pixels so resolution changes
 * need no conversion.
 * This experiment is removable as its files plus its view table row.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "render/camera.h"
#include "render/r3d_instance.h"
#include "render/raster_attachment.h"

/* A pixel's motion: the previous position minus this one, in half pixels
 * of this picture, so +-63.5 pixels. RASTER_MOTION_UNKNOWN on either axis:
 * nothing drawn, no previous picture, behind the previous camera or farther
 * than that. */
typedef struct {
    int8_t dx, dy;
} raster_motion_px_t;

#define RASTER_MOTION_UNKNOWN       INT8_MIN
/* The most instances a raster with motion attached may draw. */
#define RASTER_MOTION_INSTANCES_MAX 16

/* What a previous picture left, and this picture's maps; the caller owns
 * it, zeroed, for as long as the attachment is attached. */
typedef struct {
    bool has_previous;
    camera_t camera;
    int seen_count;
    const r3d_placement_t* seen_key[RASTER_MOTION_INSTANCES_MAX];
    r3d_placement_t seen[RASTER_MOTION_INSTANCES_MAX];
    /* This picture's: map[0] carries a point of this picture's lens space
     * into the previous one's for anything that did not move, map[i + 1]
     * for instance i that did, when moved[i]. */
    bool known;      /* there was a previous picture */
    int first_moved; /* the first instance that moved, or -1 */
    bool moved[RASTER_MOTION_INSTANCES_MAX];
    float map[RASTER_MOTION_INSTANCES_MAX + 1][3][4];
    float center_x, center_y, near_z;
} raster_motion_t;

/* The attachment, storing its state in `state`. */
raster_attachment_t raster_motion_view(void* state);

/* The next picture has no previous one, as after a cut. */
void raster_motion_forget(raster_motion_t* state);

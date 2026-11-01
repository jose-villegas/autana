/*
 * raster_motion: previous-position offsets from view depth and previous
 * instance models, in current-picture pixels so resolution changes
 * need no conversion.
 * This experiment is removable as its files plus its view table row.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "build/build_variant.h"

#include "math/linear/mat4f.h"
#include "math/projection/projectf.h"
#include "render/r3d_instance.h"
#include "render/r3d_pipeline.h"
#include "render/raster_attachment.h"
#include "render/render_view.h"

/* A pixel's motion: the previous position minus this one, in half pixels
 * of this picture, so +-63.5 pixels. RASTER_MOTION_UNKNOWN on either axis:
 * nothing drawn, no previous picture, behind the previous view or farther
 * than that. */
typedef struct {
    int8_t dx, dy;
} raster_motion_px_t;

#define RASTER_MOTION_UNKNOWN INT8_MIN

/* While drawing, a pixel's tag: 0 for anything that did not move, i + 1 for
 * instance i that did. */
typedef uint16_t raster_motion_tag_t;
/* The most instances a picture can tag. */
#define RASTER_MOTION_INSTANCES_MAX ((int)UINT16_MAX)

/* What a previous picture left, and this picture's maps; the caller owns
 * it, zeroed, for as long as the attachment is attached, and gives it room
 * for each picture's instances with raster_motion_reserve(). */
typedef struct {
    bool has_previous;
    render_view_t previous;
    int capacity; /* instances the arrays below hold */
    void* block;  /* their one allocation; raster_motion_release() frees it */
    int seen_count;
    const mat4f_t** seen_key;
    mat4f_t* seen;
    /* This picture's: map[0] carries a point of this picture's lens space
     * into the previous one's for anything that did not move, map[i + 1]
     * for instance i that did, when moved[i]. */
    bool known;      /* there was a previous picture */
    int first_moved; /* the first instance that moved, or -1 */
    bool* moved;
    mat4f_t* map; /* capacity + 1 */
    projectf_t project;
    /* A map's two lenses, here rather than on the drawing task's stack. */
    r3d_lens_t now, before;
} raster_motion_t;

/* The attachment, storing its state in `state`. */
raster_attachment_t raster_motion_view(void* state);

/* The next picture has no previous one, as after a cut. */
void raster_motion_forget(raster_motion_t* state);

/* Room for pictures of `instances`, keeping the history; one compare when it
 * already fits. False, the state untouched, when the room cannot be had. */
bool raster_motion_reserve(raster_motion_t* state, int instances);

/* Frees the room, the one place it is freed; the state is zeroed. */
void raster_motion_release(raster_motion_t* state);

/* The view row's hooks over raster_motion_reserve() and raster_motion_release(). */
bool raster_motion_reserve_view(void* state, int instances);
void raster_motion_release_view(void* state);

#if BUILD_VARIANT_DEVELOPMENT_OR_HOST
bool raster_motion_copy_view(void* destination, const void* source, const raster_t* raster);
#endif

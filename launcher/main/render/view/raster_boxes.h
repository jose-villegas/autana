/*
 * raster_boxes: bounding boxes over the shaded picture, as a development
 * view. Each box is coloured by its shape, so copies of one shape share a
 * colour, and a stretch of edge behind what was drawn shows dimmed.
 *
 * Two layers: every instance the raster draws (its root box under its
 * model; instances of one mesh are one shape) and, when a caller gives them,
 * the source objects an import detected, in world units.
 *
 * Tunables: boxview.copies (the fewest copies a shape needs for its boxes to
 * be drawn; 1 draws every box) and boxview.behind (raster_boxes_behind_t).
 */
#pragma once

#include <stdint.h>

#include "math/geometry/box3f.h"
#include "render/raster.h"
#include "render/render_view.h"

typedef enum {
    RASTER_BOXES_BEHIND_HIDE, /* an edge behind what was drawn is left out */
    RASTER_BOXES_BEHIND_DIM,  /* drawn at RASTER_LINES_BEHIND_KEEP */
    RASTER_BOXES_BEHIND_FULL, /* drawn as if nothing hid it */
    RASTER_BOXES_BEHIND_COUNT,
} raster_boxes_behind_t;

/* boxview.copies' default: only shapes with copies are drawn. */
#define RASTER_BOXES_COPIES_DEFAULT 2

typedef struct {
    box3f_t box;     /* world units, the renderer's placement applied */
    uint16_t shape;  /* copies of one shape share it */
    uint16_t copies; /* how many boxes share the shape */
} raster_box_t;

typedef struct {
    const raster_t* raster; /* the frame's instances, from its draw */
    render_view_t camera;
    const raster_box_t* objects; /* the caller's, NULL when it gave none */
    int object_count;
} raster_boxes_t;

/* Draws nothing of its own: the shading stays, the boxes come in the overlay. */
raster_attachment_t raster_boxes_view(void* state);

/* Box edges over the composed picture, `width` by `height` from (0, 0). */
void raster_boxes_overlay(void* state, int width, int height);

/* The source objects' boxes: the caller's, kept while the view is attached. */
void raster_boxes_set_objects(raster_boxes_t* state, const raster_box_t* boxes, int count);

/*
 * raster_boxes: bounding boxes over the shaded picture, as a development
 * view. Each box is coloured by its shape, so copies of one shape share a
 * colour, and a stretch of edge behind what was drawn shows dimmed.
 *
 * Two layers: every instance the raster draws (its root box under its
 * model; instances of one mesh are one shape) and, when a caller gives them,
 * the source objects an import detected, in world units.
 */
#pragma once

#include <stdint.h>

#include "math/geometry/box3f.h"
#include "render/raster.h"
#include "render/render_view.h"

typedef struct {
    box3f_t box;    /* world units, the renderer's placement applied */
    uint16_t shape; /* copies of one shape share it */
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

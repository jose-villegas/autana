/*
 * raster_boxes: the frame's instances as boxes over the shaded picture, as a
 * development view. Each instance the raster drew shows its root box under
 * its model, coloured by its mesh, so copies of one mesh share a colour, and
 * a stretch of edge behind what was drawn is left out or dimmed.
 *
 * Tunables: boxview.copies (the fewest instances a mesh needs for its boxes
 * to be drawn; 1 draws every instance) and boxview.behind (raster_boxes_behind_t).
 */
#pragma once

#include "render/raster.h"
#include "render/render_view.h"

typedef enum {
    RASTER_BOXES_BEHIND_HIDE, /* an edge behind what was drawn is left out */
    RASTER_BOXES_BEHIND_DIM,  /* drawn at RASTER_LINES_BEHIND_KEEP */
    RASTER_BOXES_BEHIND_FULL, /* drawn as if nothing hid it */
    RASTER_BOXES_BEHIND_COUNT,
} raster_boxes_behind_t;

/* boxview.copies' default: every instance is drawn. */
#define RASTER_BOXES_COPIES_DEFAULT 1

typedef struct {
    const raster_t* raster; /* the frame's instances, from its draw */
    render_view_t camera;
} raster_boxes_t;

/* Draws nothing of its own: the shading stays, the boxes come in the overlay. */
raster_attachment_t raster_boxes_view(void* state);

/* Box edges over the composed picture, `width` by `height` from (0, 0). */
void raster_boxes_overlay(void* state, int width, int height);

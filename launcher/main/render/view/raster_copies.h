/*
 * raster_copies: each shape the frame draws at least RASTER_COPIES_SHARED
 * times in a hue of its own, every copy of it alike; a shape drawn once is
 * grey. Copies are the instances whose meshes map the same positions, as when
 * one imported mesh is drawn by several renderers.
 */
#pragma once

#include <stdint.h>

#include "gfx/target/gfx_color.h"
#include "image/pixel/rgb565.h"
#include "render/raster.h"
#include "render/raster_attachment.h"

/* The fewest instances drawing a shape that make them copies. */
#define RASTER_COPIES_SHARED 2
/* The id a shape drawn fewer than RASTER_COPIES_SHARED times takes; instance
 * indices stay below it. */
#define RASTER_COPIES_UNIQUE UINT16_MAX
/* What the view paints RASTER_COPIES_UNIQUE in. */
#define RASTER_COPIES_GREY   RGB565BE(0x808080)

typedef struct {
    const raster_t* raster; /* the frame's instances, from its draw */
} raster_copies_t;

raster_attachment_t raster_copies_view(void* state);

/* The first instance drawing instance `i`'s mesh, and how many instances draw
 * it. Each renderer holds its own view of a mesh, so copies are the instances
 * whose views map the same positions. A skinned or dynamic mesh writes its
 * own positions, so it is a shape of its own. */
int raster_copies_shape(const raster_t* raster, int i, int* copies);

/* raster_meshlets: cluster and triangle IDs painted as disjoint per-instance flat hues. */
#pragma once

#include <stdint.h>

#include "gfx/target/gfx_color.h"
#include "render/raster_attachment.h"

typedef struct {
    uint32_t next;
    const raster_t* raster;
} raster_meshlets_t;

raster_attachment_t raster_meshlets_view(void* state);
raster_attachment_t raster_triangles_view(void* state);

/* The flat hue the view paints `id` in; ids from 1. */
gfx_color_t raster_meshlets_color(uint32_t id);

/* raster_triangles: disjoint per-instance triangle IDs painted as flat hues. */
#pragma once

#include <stdint.h>

#include "gfx/target/gfx_color.h"
#include "render/raster_attachment.h"

typedef struct {
    uint32_t next;
    const raster_t* raster;
} raster_triangles_t;

raster_attachment_t raster_triangles_view(void* state);

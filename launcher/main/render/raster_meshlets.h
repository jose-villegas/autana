/* raster_meshlets: disjoint per-instance cluster IDs painted as flat hues. */
#pragma once

#include <stdint.h>

#include "render/raster_attachment.h"

typedef struct {
    uint32_t next;
    const raster_t* raster;
} raster_meshlets_t;

raster_attachment_t raster_meshlets_view(void* state);

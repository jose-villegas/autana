/* raster_meshlets: disjoint per-instance cluster IDs painted as flat hues. */
#pragma once

#include "build/build_variant.h"

#include <stdint.h>

#include "gfx/target/gfx_color.h"
#include "render/raster_attachment.h"

typedef struct {
    uint32_t next;
    const raster_t* raster;
} raster_meshlets_t;

raster_attachment_t raster_meshlets_view(void* state);

/* The flat hue the view paints `id` in; ids from 1. */
gfx_color_t raster_meshlets_color(uint32_t id);

#if BUILD_VARIANT_DEVELOPMENT_OR_HOST
bool raster_meshlets_copy_view(void* destination, const void* source, const raster_t* raster);
#endif

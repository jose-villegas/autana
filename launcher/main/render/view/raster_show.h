/* raster_show: development views paint colour from the raster maps. */
#pragma once

#include "render/raster.h"

/* The tile a hierarchical depth test would cull by. */
#define RASTER_SHOW_TILE 8

raster_attachment_t raster_depth_view(void* state);
raster_attachment_t raster_depth_tiles_view(void* state);

/* Development builds only: paints attached views before upscale. */
void raster_show(const raster_t* raster);
/* Paints colour from attachment `index`, retaining its map and the depth. */
void raster_show_map(const gfx_render_target_t* picture, int index, uint16_t clear,
                     gfx_color_t (*color_of)(const void* pixel, uint16_t clear));

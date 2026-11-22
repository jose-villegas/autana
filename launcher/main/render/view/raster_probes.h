/* raster_probes: baked irradiance spheres over the composed picture. */
#pragma once

#include "render/probe/sh_probes.h"
#include "render/raster.h"

/* probeview.size is a percentage of the smallest grid spacing. */
#define RASTER_PROBES_SIZE_UNIT       100
#define RASTER_PROBES_SIZE_DEFAULT    12
#define RASTER_PROBES_SIZE_MAX        50
#define RASTER_PROBES_CROSS_RADIUS_PX 2
#define RASTER_PROBES_INSIDE_COLOR    RGB565BE(0xFF40FF)

typedef struct {
    const raster_t* raster;
    render_view_t camera;
    const sh_probes_t* probes;
    r3d_skin_light_transfer_t fallback_transfer;
    float fallback_white;
    bool fallback_ready;
} raster_probes_t;

raster_attachment_t raster_probes_view(void* state);
void raster_probes_overlay(void* state, int width, int height);

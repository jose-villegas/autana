/* r3d_stages: stable frame-cost bracket names for the renderer and its measurements. */
#pragma once

#define R3D_STAGE_CULL_NAME      "r3d.cull"
#define R3D_STAGE_TRANSFORM_NAME "r3d.transform"
#define R3D_STAGE_DRAW_NAME      "r3d.draw"
#define R3D_STAGE_UPSCALE_NAME   "r3d.upscale"

enum { R3D_STAGE_CULL, R3D_STAGE_TRANSFORM, R3D_STAGE_DRAW, R3D_STAGE_UPSCALE, R3D_STAGE_COUNT };

static const char* const r3d_stage_names[R3D_STAGE_COUNT] = {
    R3D_STAGE_CULL_NAME,
    R3D_STAGE_TRANSFORM_NAME,
    R3D_STAGE_DRAW_NAME,
    R3D_STAGE_UPSCALE_NAME,
};

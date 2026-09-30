/*
 * sponza_flythrough - the camera loop through Sponza's atrium: in from the
 * east arcade at eye height, down the atrium, up past the galleries and
 * back, and the lens and render size it is seen through. Model units are
 * centimetres; y is up.
 */
#pragma once

#include <stdint.h>

#include "gfx/gfx.h"
#include "render/r3d_lit_pipeline.h"
#include "render/r3d_path.h"

/* Half the panel's resolution in each axis, doubled on the way out. */
#define SPONZA_RENDER_WIDTH         (GFX_WIDTH / 2)
#define SPONZA_RENDER_HEIGHT        (GFX_HEIGHT / 2)

/* The camera keeps at least this far from every triangle, so the near
 * plane never cuts into a wall; suite_sponza.c holds the path to it. */
#define SPONZA_FLYTHROUGH_CLEARANCE 25.0f

#define SPONZA_HALF_FOV_SHORT_TAN   0.62f
#define SPONZA_NEAR_Z               6.0f

extern const r3d_path_t sponza_flythrough;

/* The view t_ms into the loop at the render size, turned for `quarter`. */
void sponza_view_at(r3d_lit_view_t* view, uint32_t t_ms, int position_scale, int quarter);

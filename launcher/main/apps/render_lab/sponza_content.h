/*
 * sponza_content: the Sponza scene's id and five bakes, the size it is seen
 * at, and what its camera loop is held to. The loop is the scene's camera, its
 * lens and path from scene_camera_lens() once the scene has loaded. Model
 * units are centimetres; y is up.
 */
#pragma once

#include "gfx/gfx.h"

/* The scene's id, and its pack's name. */
#define SPONZA_SCENE                "sponza"

/* Half the panel's resolution in each axis, upscaled on the way out. */
#define SPONZA_RENDER_WIDTH         (GFX_WIDTH / 2)
#define SPONZA_RENDER_HEIGHT        (GFX_HEIGHT / 2)

/* The camera keeps at least this far from every triangle, so the near
 * plane never cuts into a wall; suite_sponza.c holds the path to it. */
#define SPONZA_FLYTHROUGH_CLEARANCE 25.0f

/* The loop is measured at a pose this often, from its start. */
#define SPONZA_POSE_EVERY_MS        5000

typedef enum {
    SPONZA_BAKE_FULL,
    SPONZA_BAKE_FLAT,
    SPONZA_BAKE_LITE,
    SPONZA_BAKE_FITTED,
    SPONZA_BAKE_FITTED_FULL,
    SPONZA_BAKE_COUNT,
} sponza_bake_t;

/* The name of the entity that draws each bake, found at load. */
extern const char* const sponza_bakes[SPONZA_BAKE_COUNT];

/*
 * sponza_content: the Sponza scene's id and its bakes, the size it is seen
 * at, and what its camera loop is held to. The loop is the scene's camera:
 * its lens from scene_camera_lens() and its entity's animated transform once
 * the scene has loaded. Model units are centimetres; y is up.
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

/* The frame-cost rows' pose spacing. */
#define SPONZA_PERF_POSE_EVERY_MS   500

#include "apps/render_lab/demo_baked_variant.h"

/* The name of the entity that draws each bake, found at load. */
extern const char* const sponza_bakes[DEMO_BAKED_VARIANT_COUNT];

/* The camera a start with none named follows; the tour is the other. */
#define SPONZA_CAMERA       "camera"
#define SPONZA_CAMERA_COUNT 2
extern const char* const sponza_cameras[SPONZA_CAMERA_COUNT];

/*
 * sponza_flythrough, the camera loop through Sponza's atrium, and the render
 * size it is seen at. Model units are centimetres; y is up. The lens and
 * path are the scene's camera object (sponza_scene_generated.h).
 */
#pragma once

#include <stdint.h>

#include "asset/asset_pack.h"
#include "gfx/gfx.h"
#include "render/r3d.h"

/* Half the panel's resolution in each axis, upscaled on the way out. */
#define SPONZA_RENDER_WIDTH         (GFX_WIDTH / 2)
#define SPONZA_RENDER_HEIGHT        (GFX_HEIGHT / 2)

/* The camera keeps at least this far from every triangle, so the near
 * plane never cuts into a wall; suite_sponza.c holds the path to it. */
#define SPONZA_FLYTHROUGH_CLEARANCE 25.0f

/* The loop is measured at a pose this often, from its start. */
#define SPONZA_POSE_EVERY_MS        5000

uint32_t sponza_flythrough_period_ms(void);

/* The eye and look direction t_ms into the loop, which wraps at the period. */
void sponza_flythrough_sample(uint32_t t_ms, vec3f_t* eye, vec3f_t* forward);

/* The camera t_ms into the loop. */
camera_t sponza_camera_at(uint32_t t_ms);

/* Opens the scene's three meshes from the asset pack, which the scene's
 * instances then draw. Returns what r3d_scene_bind() does: the first mesh
 * that is missing or malformed fails it and `failed` names it. */
asset_status_t sponza_open_meshes(const char** failed);

/* render_lab_view: context view, scale and frame-budget tunables. */
#pragma once

#include "render/context/render_context.h"
#include "scene/scene.h"

#define RENDER_LAB_SCALE_UNIT    100
#define RENDER_LAB_SCALE_DEFAULT (2 * RENDER_LAB_SCALE_UNIT)
#define RENDER_LAB_SCALE_MAX     (8 * RENDER_LAB_SCALE_UNIT)

/* The slowest render_lab.orbit_ms turn: one a minute. */
#define RENDER_LAB_ORBIT_MAX_MS  60000

/* The selected render context view, RENDER_DEBUG_VIEW_SHADED by default. */
int render_lab_debug_view(void);

/* The tunable render_lab.scale in hundredths: 200 renders at half size. */
int render_lab_scale(void);

camera_present_t render_lab_present(void);
small_fill_t render_lab_small_fill(void);
bool render_lab_apply_camera_settings(scene_t* scene, scene_entity_t camera);

/* Applies the tunables to destination dimensions. */
resolution_step_t render_lab_render_size(int width, int height);
void render_lab_apply_raster_settings(void);

/* The tunable render_lab.budget: 0 draws at render_lab.scale; otherwise
 * the milliseconds dynamic resolution holds a lit-mesh scene's draw to. */
int render_lab_budget_ms(void);

/* The tunable render_lab.orbit_ms: 0 holds an orbiting camera where it is
 * steered; otherwise it also turns once about its subject every orbit_ms. */
int render_lab_orbit_ms(void);

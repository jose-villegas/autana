/* render_lab_view: Render Lab's own tunables: the context view, small fill,
 * upscale, frame budget and frame generation. Its scale, present, layout and
 * orbit_ms are the lab's (lab/lab_view.h), held here so the portable code
 * reads them without the app. */
#pragma once

#include "core/tune.h"
#include "lab/lab_view.h"
#include "render/context/render_context.h"
#include "scene/scene.h"

/* The lab's name, the prefix of every Render Lab tunable. */
#define RENDER_LAB_NAME "render_lab"

/* The lab's view tunables, "render_lab.scale" ...: its lab_t points here, and
 * this file registers them, so the portable code and its tests read them
 * without the app. */
extern lab_view_t render_lab_view_tunables;

/* The selected render context view, RENDER_DEBUG_VIEW_SHADED by default. */
int render_lab_debug_view(void);

small_fill_t render_lab_small_fill(void);

/* The menu's small-fill choice: every SMALL_FILL_ preset. select writes its
 * three tunables, which the scene camera reads every frame, so a console SET
 * or a saved tune takes effect too; selected is the preset last picked. */
const char* const* render_lab_small_fill_names(int* count);
int render_lab_small_fill_selected(void);
bool render_lab_small_fill_select(int index);

/* The menu's upscale choice, render_lab.upscale: SHARP keeps nearest, SMOOTH
 * blends within a surface (UPSCALE_FILTER_SURFACE). */
const char* const* render_lab_upscale_names(int* count);
int render_lab_upscale_selected(void);
bool render_lab_upscale_select(int index);

bool render_lab_apply_camera_settings(entity_t camera);

/* Applies the tunables to destination dimensions. */
resolution_step_t render_lab_render_size(int width, int height);
void render_lab_apply_raster_settings(void);

/* The tunable render_lab.budget: 0 draws at render_lab.scale; otherwise
 * the milliseconds dynamic resolution holds the scene's draw to
 * (render_lab_dynres.h). */
#define RENDER_LAB_BUDGET_MAX_MS 200
int render_lab_budget_ms(void);
bool render_lab_budget_set_ms(int budget_ms);

/* The tunable render_lab.framegen: 1 follows each drawn frame with one
 * warped from it (render/framegen) in a scene that declares
 * generates_frames, 0 draws every frame. */
bool render_lab_framegen_on(void);
bool render_lab_framegen_set_on(bool on);

/* The tunable render_lab.curves, the menu's CURVES toggle: 1 draws the curves
 * a mesh's bake carries (render_view_t.curves), 0, the default, skips them. */
bool render_lab_curves_on(void);
bool render_lab_curves_set_on(bool on);

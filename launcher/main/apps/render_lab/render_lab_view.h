/* render_lab_view: context view, scale and frame-budget tunables. */
#pragma once

#include "core/tune.h"
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

/* The menu's small-fill choice: every SMALL_FILL_ preset. select writes its
 * three tunables, which the scene camera reads every frame, so a console SET
 * or a saved tune takes effect too; selected is the preset last picked. */
const char* const* render_lab_small_fill_names(int* count);
int render_lab_small_fill_selected(void);
bool render_lab_small_fill_select(int index);

bool render_lab_apply_camera_settings(scene_t* scene, scene_entity_t camera);

/* The tunable render_lab.layout, the framebuffer layout render_lab asks for
 * at enter(); a change takes effect at the next enter or scene switch. */
typedef enum {
    RENDER_LAB_LAYOUT_FULL,
    RENDER_LAB_LAYOUT_DOUBLE,
    RENDER_LAB_LAYOUT_DOUBLE_CLEAR, /* double, the sent frame filled by core 1 */
    RENDER_LAB_LAYOUT_COUNT,
} render_lab_layout_t;

render_lab_layout_t render_lab_layout(void);

/* Applies the tunables to destination dimensions. */
resolution_step_t render_lab_render_size(int width, int height);
void render_lab_apply_raster_settings(void);

/* The tunable render_lab.budget: 0 draws at render_lab.scale; otherwise
 * the milliseconds dynamic resolution holds the scene's draw to
 * (render_lab_dynres.h). */
#define RENDER_LAB_BUDGET_MAX_MS 200
int render_lab_budget_ms(void);
bool render_lab_budget_set_ms(int budget_ms);

/* The tunable render_lab.orbit_ms: 0 holds an orbiting camera where it is
 * steered; otherwise it also turns once about its subject every orbit_ms. */
int render_lab_orbit_ms(void);

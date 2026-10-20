/* render_lab_view: context view, scale and frame-budget tunables. */
#pragma once

#include "render/context/render_context.h"

#define RENDER_LAB_SCALE_UNIT    100
#define RENDER_LAB_SCALE_DEFAULT (2 * RENDER_LAB_SCALE_UNIT)
#define RENDER_LAB_SCALE_MAX     (8 * RENDER_LAB_SCALE_UNIT)

/* The selected render context view, RENDER_DEBUG_VIEW_SHADED by default. */
int render_lab_debug_view(void);

/* The tunable render_lab.scale in hundredths: 200 renders at half size. */
int render_lab_scale(void);

camera_present_t render_lab_present(void);

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
 * the milliseconds dynamic resolution holds a lit-mesh scene's draw to. */
int render_lab_budget_ms(void);

/* render_lab_dynres: dynamic resolution for whichever scene is showing,
 * from the tunable render_lab.budget. */
#pragma once

#include "render/resolution/resolution.h"

/* Rows and span setup follow height, so the ladder reduces height before
 * width. Widths use fast upscale paths; the board fit from
 * suite_raster_scale_perf.c includes one upscale per step. The fit prices
 * the raster from the triangles culling kept, so it carries to any mesh,
 * and the live refit learns what a mesh costs beyond it.
 * The 3x-cost step is recovery-only. */
#define RENDER_LAB_LADDER_STEPS    7
#define RENDER_LAB_LADDER_RECOVERY 6
#define RENDER_LAB_LADDER_HALF     4
extern const resolution_step_t render_lab_ladder[RENDER_LAB_LADDER_STEPS];
extern const resolution_model_t render_lab_ladder_model;

/* Hands render_lab.budget to the main context when it changed since the
 * last call: 0 returns it to the fixed scale. */
void render_lab_dynres_apply(void);

/* Under a budget, the size the last frame drew at as "WxH " for a scene's
 * status line, so a step shows on the panel; "" at the fixed scale. */
const char* render_lab_dynres_size(void);

/* Returns the context to the fixed scale and forgets the budget handed, so
 * the next apply hands it again. */
void render_lab_dynres_stop(void);

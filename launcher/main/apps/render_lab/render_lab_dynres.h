/* render_lab_dynres: dynamic resolution for whichever scene is showing,
 * from the tunable render_lab.budget. */
#pragma once

#include <stdbool.h>

#include "render/resolution/resolution.h"

/* Rows and span setup follow height, so the ladder reduces height before
 * width. Below the half size every step composes into the half picture,
 * and the floor halves its height. The fit, on every mesh scene at every
 * step (render_lab_dynres_steps_suite), includes each step's compose and
 * prices the raster from the triangles culling kept, so it carries to any
 * mesh; the live refit learns the rest. No step is recovery-only. */
#define RENDER_LAB_LADDER_STEPS 8
#define RENDER_LAB_LADDER_HALF  4
extern const resolution_step_t render_lab_ladder[RENDER_LAB_LADDER_STEPS];
extern const resolution_model_t render_lab_ladder_model;

/* The frame time the menu's toggle holds: the chosen size (render_lab.scale)
 * is the ladder's top, and a frame drops below it only past this floor. */
#define RENDER_LAB_FRAME_TARGET_MS   33
/* What a frame pays outside the draw and upscale the context times
 * (present, scene update, skinning), measured on every mesh scene. */
#define RENDER_LAB_FRAME_OVERHEAD_MS 10
/* The budget handed to the context: the scaled part's share of the frame. */
#define RENDER_LAB_TARGET_MS         (RENDER_LAB_FRAME_TARGET_MS - RENDER_LAB_FRAME_OVERHEAD_MS)

/* The menu's DYNAMIC RES toggle over render_lab.budget: on is any budget,
 * set_on writes RENDER_LAB_TARGET_MS or 0. */
bool render_lab_dynres_on(void);
bool render_lab_dynres_set_on(bool on);

/* Hands render_lab.budget to the main context, topped at the size
 * render_lab.scale chooses, when either changed since the last call: a
 * budget of 0 returns it to the fixed scale. */
void render_lab_dynres_apply(void);

/* Under a budget, the size the last frame drew at as "WxH " for a scene's
 * status line, so a step shows on the panel; "" at the fixed scale. */
const char* render_lab_dynres_size(void);

/* Returns the context to the fixed scale and forgets the budget handed, so
 * the next apply hands it again. */
void render_lab_dynres_stop(void);

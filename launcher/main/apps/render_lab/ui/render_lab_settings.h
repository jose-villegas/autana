/* render_lab_settings: Render Lab's own rows in the lab menu, under the scene's. */
#pragma once

#include "lab/lab_scene.h"

/* The SMALL FILL row's caption, and its dropdown's identity. */
#define RENDER_LAB_SMALL_FILL_LABEL  "SMALL FILL:"
#define RENDER_LAB_UPSCALE_LABEL     "UPSCALE:"
#define RENDER_LAB_DYNAMIC_RES_LABEL "DYNAMIC RES"
#define RENDER_LAB_FRAME_GEN_LABEL   "FRAME GEN"
#define RENDER_LAB_TEXTURES_LABEL    "TEXTURES"

/* The small-fill and upscale dropdowns, the frame generation toggle when
 * `showing` generates frames, and the dynamic resolution toggle, each `width`
 * wide (a lab_settings_fn). */
void render_lab_settings_draw(const lab_scene_t* showing, int width);

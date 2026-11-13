/* render_lab_settings: Render Lab's own rows in the lab menu, under the scene's. */
#pragma once

/* The SMALL FILL row's caption, and its dropdown's identity. */
#define RENDER_LAB_SMALL_FILL_LABEL  "SMALL FILL:"
#define RENDER_LAB_UPSCALE_LABEL     "UPSCALE:"
#define RENDER_LAB_DYNAMIC_RES_LABEL "DYNAMIC RES"
#define RENDER_LAB_CURVES_LABEL      "CURVES"

/* The small-fill and upscale dropdowns and the dynamic resolution and curves toggles,
 * each `width` wide (a lab_controls_fn). */
void render_lab_settings_draw(int width);

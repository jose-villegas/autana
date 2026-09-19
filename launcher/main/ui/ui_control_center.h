#pragma once

#include "app.h"
#include "microui.h"
#include "ui/control_center_layout_generated.h"

/* The baked layout matching the current ui_width() x ui_height(). */
const control_center_layout_t* ui_control_center_baked_layout(void);

/* The widgets alone, with no ui_begin()/ui_end() around them. */
void ui_control_center_draw(mu_Context* ctx, const control_center_layout_t* layout);

/* Paints over whatever the framebuffer holds: call
 * ui_control_center_dim_backdrop() once over the screen underneath first. */
void ui_control_center_frame(const input_t* input);

/* Host previews pass authored geometry; firmware uses the baked table. */
void ui_control_center_frame_layout(const input_t* input, const control_center_layout_t* layout);

void ui_control_center_dim_backdrop(void);

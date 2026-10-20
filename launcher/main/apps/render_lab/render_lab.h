/* render_lab: shared scene settings and status length. */
#pragma once

#include <stdbool.h>

#define RENDER_LAB_BACKGROUND_RGB 0x0A0C14
#define RENDER_LAB_STATUS_LEN     48

extern const char* render_lab_start_scene_key;

/* A family's scenes for its own dropdown: their variant names, and the index
 * of the one showing (-1 if another family is). Returns the count. */
int render_lab_family_variants(const char* family, const char* const** names, int* selected);
/* Switches to the family's index-th scene at the next frame. */
bool render_lab_request_variant(const char* family, int index);
extern bool render_lab_show_hud;

/* NULL selects the scene's first camera. */
extern const char* render_lab_start_camera;

/* render_lab: shared scene background and entry camera selection. */
#pragma once

#include <stdbool.h>

#define RENDER_LAB_BACKGROUND_RGB 0x0A0C14
#define RENDER_LAB_STATUS_LEN     48

extern const char* render_lab_start_scene_key;
extern bool render_lab_show_hud;

/* NULL selects the scene's first camera. */
extern const char* render_lab_start_camera;

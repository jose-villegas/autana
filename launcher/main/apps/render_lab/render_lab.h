/* render_lab: the app's lab (lab/lab.h) and its scenes. */
#pragma once

#include <stdbool.h>

#include "app/app.h"
#include "lab/lab.h"

/* Its start scene, start camera and HUD switch are the lab's fields:
 * render_lab.start_key, .start_camera and .show_hud. */
extern lab_t render_lab;
extern app_t app_render_lab;

/* The app's `render` console command, given the words after `render`, such
 * as "clip walk 400"; replies go to stdout. A host render calls it directly,
 * since a host build has no console. */
bool render_lab_console(const char* args);

extern const lab_scene_t scene_capybara;
extern const lab_scene_t scene_water;

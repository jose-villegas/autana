/* render_lab: shared scene settings and status length. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define RENDER_LAB_STATUS_LEN 48

extern const char* render_lab_start_scene_key;

extern bool render_lab_show_hud;

/* NULL selects the scene's first camera. */
extern const char* render_lab_start_camera;

/* The app's `render` console command, given the words after `render`, such
 * as "clip walk 400"; replies go to stdout. A host render calls it directly,
 * since a host build has no console. */
bool render_lab_console(const char* args);

/* The showing scene's path clip, as its `path` hook names it: its length in
 * ms, 0 when the scene has none, has not loaded or lacks that clip. */
uint32_t render_lab_path_duration_ms(void);

/* Places the showing scene's path at `t_ms`, wrapped to its length, cutting to
 * its clip first; a frame at dt 0 then draws that pose. False when the scene
 * has no path. */
bool render_lab_seek_path(uint32_t t_ms);

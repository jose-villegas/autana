/*
 * render_lab_view: how a lit-mesh scene shows its frame. Apart from
 * render_lab.h so that a scene which sets small3dlib's options before its own
 * includes does not meet the renderer's first.
 */
#pragma once

#include <stdbool.h>

#include "render/r3d.h"

/* The tunable render_lab.view: as shaded, or as the depth the frame left
 * (raster_show()). Constant RASTER_SHOW_SHADED when tunables are. */
raster_show_t render_lab_view(void);

/* Whether the running scene honours it (render_lab_scene_t). */
bool render_lab_scene_shows_views(void);

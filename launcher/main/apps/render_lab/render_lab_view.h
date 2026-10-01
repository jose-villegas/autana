/*
 * render_lab_view: how a lit-mesh scene shows its frame, apart from
 * render_lab.h.
 */
#pragma once

#include <stdbool.h>

#include "render/r3d.h"

/* The tunable render_lab.view: as shaded, or as the depth the frame left
 * (raster_show()). Constant RASTER_SHOW_SHADED when tunables are. */
raster_show_t render_lab_view(void);

/* The tunable render_lab.scale in hundredths: 200 renders at half size. */
int render_lab_scale(void);

/* Whether the running scene honours it (render_lab_scene_t). */
bool render_lab_scene_shows_views(void);

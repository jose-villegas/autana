/*
 * render_lab_view: how a lit-mesh scene shows its frame. Apart from
 * render_lab.h so that a scene which sets small3dlib's options before its own
 * includes does not meet the renderer's first.
 */
#pragma once

#include <stdbool.h>

#include "render/r3d_lit_frame.h"

/* The tunable render_lab.view: as shaded, or as the depth the frame left
 * (r3d_lit_frame_show()). Constant R3D_LIT_VIEW_SHADED when tunables are. */
r3d_lit_view_mode_t render_lab_view(void);

/* Whether the running scene honours it (render_lab_scene_t). */
bool render_lab_scene_shows_views(void);

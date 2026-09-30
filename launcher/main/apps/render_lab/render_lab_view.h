/*
 * render_lab_view - how a lit-mesh scene shows its frame. Apart from render_lab.h so that a
 * scene which sets small3dlib's options before its own includes does not
 * meet the renderer's first.
 */
#pragma once

#include <stdbool.h>

#include "render/r3d_lit_frame.h"

/* Development builds, and the host render, which links the same scenes but
 * cannot compile the panel's development-only code. */
#ifndef RENDER_LAB_VIEWS
#define RENDER_LAB_VIEWS CONFIG_LAUNCHER_DEVELOPMENT
#endif

#if RENDER_LAB_VIEWS
/* As shaded, or as the depth the frame left (r3d_lit_frame_show()). Read
 * every frame by any scene whose render_lab_scene_t sets shows_view_modes. */
extern r3d_lit_view_mode_t render_lab_view_mode;

/* Whether the running scene honours it. */
bool render_lab_scene_shows_views(void);
#endif

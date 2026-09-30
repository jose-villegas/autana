/*
 * render_lab_mesh_frame - the one look a mesh scene gives outside itself: the
 * frame it last rendered, for the host render's depth view.
 */
#pragma once

#include "render/r3d_lit_frame.h"

/* The colour and depth targets of the running mesh scene, or NULL when none
 * is entered. Read-only. */
const r3d_lit_frame_t* render_lab_mesh_frame(void);

/* camera: the camera component, a lens (lens.h) posed by its entity's world transform. */
#pragma once

#include <stdbool.h>

#include "scene/camera/lens.h"
#include "scene/entity/entity.h"

/* The first camera within `within`, in load order, or ENTITY_NONE. */
entity_t camera_first(entity_t within);

/* The camera's lens, or NULL when the entity has no camera. */
const lens_t* camera_lens(entity_t camera);

/* Draws through `camera` from now on. False when the entity has no camera. */
bool camera_activate(entity_t camera);

/* The camera drawn now, for a caller that moves whichever camera is drawn;
 * ENTITY_NONE when no camera is active. */
entity_t camera_active(void);

/* For a caller that jumps `camera` when it is the active one: the renderer
 * drops what it carried from earlier frames, as it does on activation and on
 * a seek. Smaller moves converge within a few frames and need no cut. */
void camera_cut(entity_t camera);

/* No camera is active, so nothing is drawn, until the next camera_activate(). */
void camera_deactivate(void);

/* False for a non-camera entity or an invalid value; refusals change nothing. */
bool camera_set_present(entity_t camera, camera_present_t present);
bool camera_set_small_fill(entity_t camera, small_fill_t fill);
/* render_view_t.skip_hidden for the camera's views. */
bool camera_set_skip_hidden(entity_t camera, bool skip);
/* render_view_t.curves for the camera's views. */
bool camera_set_curves(entity_t camera, bool curves);
/* The lens shift (render_view_t.shift): an off-axis view, as a window onto a
 * fixed rectangle needs. False also for a shift that is not finite. */
bool camera_set_shift(entity_t camera, vec2f_t shift);

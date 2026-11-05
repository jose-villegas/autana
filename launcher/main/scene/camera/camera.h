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

/* No camera is active, so nothing is drawn, until the next camera_activate(). */
void camera_deactivate(void);

/* False for a non-camera entity or an invalid value; refusals change nothing. */
bool camera_set_present(entity_t camera, camera_present_t present);
bool camera_set_small_fill(entity_t camera, small_fill_t fill);
/* render_view_t.skip_hidden for the camera's views. */
bool camera_set_skip_hidden(entity_t camera, bool skip);
/* The lens shift (render_view_t.shift): an off-axis view, as a window onto a
 * fixed rectangle needs. False also for a shift that is not finite. */
bool camera_set_shift(entity_t camera, vec2f_t shift);

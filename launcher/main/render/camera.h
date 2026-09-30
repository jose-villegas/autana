/*
 * camera: a pinhole camera in model units, as a scene places one. What a
 * renderer derives from it for a viewport is the renderer's own.
 */
#pragma once

#include "render/vec3f.h"

/* A pinhole camera. Its lens is fitted to the shorter axis of the upright
 * picture; `forward` need not be normalised but must not be vertical. */
typedef struct {
    vec3f_t eye, forward; /* model units */
    float half_fov_short_tan;
    float near_z; /* model units */
} camera_t;

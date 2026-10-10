/*
 * r3d_line_camera: a transform pose and viewport fit for points and segments
 * projected through r3d_project.h in the scene's own length unit.
 * Header-only and ESP-IDF-free; callers supply the model and viewport.
 */
#pragma once

#include "math/linear/transformf.h"
#include "render/r3d_project.h"
#include "render/viewport.h"

typedef struct {
    transformf_t pose;
    float focal; /* 0 is orthographic */
    float near_z;
} r3d_line_camera_t;

/* The scale is fitted to the viewport's SHORTER axis and used for both, so
 * pixels stay square and the longer axis simply sees further. */
static inline r3d_line_view_t
r3d_line_camera_view(r3d_line_camera_t camera, const transformf_t* model, viewport_t viewport) {
    const int fit = viewport.width < viewport.height ? viewport.width : viewport.height;

    r3d_line_view_t view;
    view.matrix = mat4f_mul(transformf_view(&camera.pose), transformf_compute_matrix(model));
    view.focal = camera.focal;
    view.near_z = camera.near_z;
    view.center_x = viewport.width / 2;
    view.center_y = viewport.height / 2;
    view.scale = (float)(fit / 2);
    return view;
}

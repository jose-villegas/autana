/*
 * r3d_line_camera: the camera for points and segments projected through
 * r3d_project.h (float, in the scene's own length unit), and the two things
 * a caller placing one needs: the roll that keeps a scene's up on the
 * shell's up, and the fit onto a non-square viewport. The pose is a
 * transformf_t, so a caller places it with the transform helpers.
 * Header-only, static inline and ESP-IDF-free, so a host suite can check
 * every line of it. A caller passes its own quarter and viewport in.
 */
#pragma once

#include "render/r3d_project.h"
#include "render/viewport.h"
#include "util/math/transformf.h"

typedef struct {
    transformf_t pose;
    float focal; /* 0 is orthographic */
    float near_z;
} r3d_line_camera_t;

/* A caller drawing in the panel's native frame, rather than through the
 * shell's UI transform, is not turned with the shell; this roll about the
 * camera's own forward axis is what keeps its up on the shell's current up.
 * It adds to the pose's roll, so the pose passed in must have none. */
static inline r3d_line_camera_t
r3d_line_camera_upright(r3d_line_camera_t camera, int quarter) {
    const quatf_t roll = quatf_from_axis_angle((vec3f_t){0.0F, 0.0F, 1.0F}, (float)quarter * (MATH_PI / 2.0F));
    transformf_rotate(&camera.pose, roll);
    return camera;
}

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

/*
 * r3d_camera - a camera DESCRIPTION in the engine's fixed-point conventions
 * (S3L_F units, angles in S3L turns), and the two pieces of logic every
 * hand-built small3dlib camera in this tree duplicated: rolling the camera
 * so a scene's up stays on the shell's up, and mapping a non-square
 * viewport (centre, scale, which axis the lens fits). Header-only, static
 * inline, and ESP-IDF-free, so a host suite can check every line of it.
 *
 * Knows nothing of `display_shell_quarter()` or `GFX_WIDTH`: a caller passes
 * its own quarter and its own viewport in.
 */
#pragma once

#include "render/r3d_project.h"

typedef struct {
    int width;
    int height;
    int quarter; /* 0..3, as display_shell_quarter() numbers a turn */
} r3d_viewport_t;

typedef struct {
    S3L_Transform3D pose;
    S3L_Unit focal; /* 0 is orthographic, as small3dlib defines it */
    S3L_Unit near_z;
} r3d_camera_t;

/* The roll wire_advance_pose() applies as `camera.rotation.z =
 * -display_shell_quarter() * (S3L_F / 4)`: a scene drawn in the panel's own
 * native frame needs this to keep its up on the shell's current up. */
static inline r3d_camera_t
r3d_camera_upright(r3d_camera_t camera, int quarter) {
    camera.pose.rotation.z = -quarter * (S3L_F / 4);
    return camera;
}

/* The world*camera matrix block boot and the wire scene both build by hand,
 * plus the viewport's centre and its shorter-axis-fit scale - one value for
 * both axes, matching r3d_view_t's own "both axes" scale. */
static inline r3d_view_t
r3d_camera_view(r3d_camera_t camera, S3L_Transform3D model_transform, r3d_viewport_t viewport) {
    S3L_Mat4 world_mat, camera_mat;
    S3L_makeWorldMatrix(model_transform, world_mat);
    S3L_makeCameraMatrix(camera.pose, camera_mat);
    S3L_mat4Xmat4(world_mat, camera_mat);

    const int fit = viewport.width < viewport.height ? viewport.width : viewport.height;

    r3d_view_t view;
    S3L_mat4Copy(world_mat, view.matrix);
    view.focal = camera.focal;
    view.near_z = camera.near_z;
    view.center_x = viewport.width / 2;
    view.center_y = viewport.height / 2;
    view.scale = fit / 2;
    return view;
}

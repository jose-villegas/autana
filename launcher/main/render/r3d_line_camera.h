/*
 * r3d_line_camera: the camera for points and segments projected through
 * small3dlib's fixed point (S3L_F units, angles in S3L turns), and the two
 * things a caller placing one needs: the roll that keeps a scene's up on the
 * shell's up, and the fit onto a non-square viewport. Unlike r3d_camera_t
 * its pose carries a roll and composes with a model transform in integers.
 * Header-only, static inline and ESP-IDF-free, so a host suite can check
 * every line of it. A caller passes its own quarter and viewport in.
 */
#pragma once

#include "render/r3d_project.h"
#include "render/r3d_viewport.h"

typedef struct {
    S3L_Transform3D pose;
    S3L_Unit focal; /* 0 is orthographic, as small3dlib defines it */
    S3L_Unit near_z;
} r3d_line_camera_t;

/* A caller drawing in the panel's native frame, rather than through the
 * shell's UI transform, is not turned with the shell; this roll is what
 * keeps its up on the shell's current up. Replaces any roll the pose had. */
static inline r3d_line_camera_t
r3d_line_camera_upright(r3d_line_camera_t camera, int quarter) {
    camera.pose.rotation.z = -quarter * (S3L_F / 4);
    return camera;
}

/* The scale is fitted to the viewport's SHORTER axis and used for both, so
 * pixels stay square and the longer axis simply sees further. */
static inline r3d_line_view_t
r3d_line_camera_view(r3d_line_camera_t camera, S3L_Transform3D model_transform, r3d_viewport_t viewport) {
    S3L_Mat4 world_mat, camera_mat;
    S3L_makeWorldMatrix(model_transform, world_mat);
    S3L_makeCameraMatrix(camera.pose, camera_mat);
    S3L_mat4Xmat4(world_mat, camera_mat);

    const int fit = viewport.width < viewport.height ? viewport.width : viewport.height;

    r3d_line_view_t view;
    S3L_mat4Copy(world_mat, view.matrix);
    view.focal = camera.focal;
    view.near_z = camera.near_z;
    view.center_x = viewport.width / 2;
    view.center_y = viewport.height / 2;
    view.scale = fit / 2;
    return view;
}

/* render_view_fixture: upright poses for legacy eye-and-forward test fixtures. */
#pragma once

#include "render/camera.h"
#include "render/render_view.h"

static inline render_view_t
render_view_fixture(const camera_t* camera, viewport_t viewport) {
    transformf_t pose = TRANSFORMF_IDENTITY;
    pose.position = camera->eye;
    transformf_look_at(&pose, vec3f_add(pose.position, camera->forward), (vec3f_t){0.0F, 1.0F, 0.0F});
    return render_view_make(&pose, camera->half_fov_short_tan, camera->near_z, viewport);
}

#include "render/r3d_scene.h"

#include <stddef.h>

r3d_placement_t
r3d_placement_from(const transformf_t* pose) {
    const mat4f_t matrix = transformf_compute_matrix(pose);
    return (r3d_placement_t){{{matrix.m[0][0], matrix.m[0][1], matrix.m[0][2]},
                              {matrix.m[1][0], matrix.m[1][1], matrix.m[1][2]},
                              {matrix.m[2][0], matrix.m[2][1], matrix.m[2][2]}},
                             pose->position};
}

void
r3d_scene_camera_sample(const r3d_scene_camera_t* camera, vec3f_t* eye, vec3f_t* forward) {
    const transformf_t* at = camera->pose;
    *eye = at == NULL ? (vec3f_t){0.0F, 0.0F, 0.0F} : at->position;
    *forward = at == NULL ? (vec3f_t){0.0F, 0.0F, 1.0F} : quatf_rotate(at->rotation, (vec3f_t){0.0F, 0.0F, 1.0F});
}

render_view_t
r3d_scene_view_at(const r3d_scene_camera_t* camera, viewport_t viewport) {
    vec3f_t position;
    vec3f_t forward;
    r3d_scene_camera_sample(camera, &position, &forward);
    const transformf_t pose = transformf_looking(position, forward, (vec3f_t){0.0F, 1.0F, 0.0F});
    return render_view_make(&pose, camera->half_fov_short_tan, camera->near_z, viewport);
}

static const anim_field_t CAMERA_FIELDS[] = {
    ANIM_FIELD(r3d_scene_camera_t, half_fov_short_tan, ANIM_VALUE_FLOAT),
    ANIM_FIELD(r3d_scene_camera_t, near_z, ANIM_VALUE_FLOAT),
};
const anim_component_fields_t R3D_SCENE_CAMERA_FIELDS = {
    ANIM_COMPONENT_CAMERA,
    CAMERA_FIELDS,
    sizeof CAMERA_FIELDS / sizeof CAMERA_FIELDS[0],
};

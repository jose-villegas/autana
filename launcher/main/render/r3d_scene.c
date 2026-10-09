#include "render/r3d_scene.h"

#include <stddef.h>

uint32_t
r3d_scene_camera_period_ms(const r3d_scene_camera_t* camera) {
    return camera->path == NULL ? 0 : camera->path->clip.duration_ms;
}

void
r3d_scene_camera_sample(const r3d_scene_camera_t* camera, uint32_t t_ms, vec3f_t* eye, vec3f_t* forward) {
    if (camera->path == NULL) {
        const r3d_placement_t* at = camera->placement;
        /* A camera looks down its own -Z: the negated third column. */
        *eye = at == NULL ? (vec3f_t){0.0F, 0.0F, 0.0F} : at->position;
        *forward = at == NULL ? (vec3f_t){0.0F, 0.0F, -1.0F} : (vec3f_t){-at->m[0][2], -at->m[1][2], -at->m[2][2]};
        return;
    }
    float position[ANIM_WIDTH_MAX];
    float turn[ANIM_WIDTH_MAX];
    float ahead[3];
    const float seconds = anim_clip_seconds(&camera->path->clip, t_ms, ANIM_LOOP);
    anim_track_sample(&camera->path->translation, seconds, position);
    anim_track_sample(&camera->path->rotation, seconds, turn);
    /* A glTF camera looks down its own -Z. */
    anim_quat_rotate(turn, (const float[3]){0.0F, 0.0F, -1.0F}, ahead);
    *eye = (vec3f_t){position[0], position[1], position[2]};
    *forward = (vec3f_t){ahead[0], ahead[1], ahead[2]};
}

render_view_t
r3d_scene_view_at(const r3d_scene_camera_t* camera, uint32_t t_ms, viewport_t viewport) {
    transformf_t pose = TRANSFORMF_IDENTITY;
    vec3f_t forward;
    r3d_scene_camera_sample(camera, t_ms, &pose.position, &forward);
    transformf_look_at(&pose, vec3f_add(pose.position, forward), (vec3f_t){0.0F, 1.0F, 0.0F});
    return render_view_make(&pose, camera->half_fov_short_tan, camera->near_z, viewport);
}

r3d_placement_t
r3d_scene_camera_placement(const transformf_t* pose) {
    const vec3f_t right = quatf_rotate(pose->rotation, (vec3f_t){1.0F, 0.0F, 0.0F});
    const vec3f_t up = quatf_rotate(pose->rotation, (vec3f_t){0.0F, 1.0F, 0.0F});
    const vec3f_t forward = quatf_rotate(pose->rotation, (vec3f_t){0.0F, 0.0F, 1.0F});
    /* Columns -right, up, -forward: a rotation still, since (-right) x up = -forward. */
    return (r3d_placement_t){{{-right.x, up.x, -forward.x}, {-right.y, up.y, -forward.y}, {-right.z, up.z, -forward.z}},
                             pose->position};
}

#include "render/r3d_scene.h"

#include <stddef.h>

#pragma GCC diagnostic error "-Wdouble-promotion"

uint32_t
r3d_scene_camera_period_ms(const r3d_scene_camera_t* camera) {
    return camera->path == NULL ? 0 : camera->path->clip->duration_ms;
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
    const float seconds = anim_clip_seconds(camera->path->clip, t_ms, ANIM_LOOP);
    anim_track_sample(camera->path->translation, seconds, position);
    anim_track_sample(camera->path->rotation, seconds, turn);
    /* A glTF camera looks down its own -Z. */
    anim_quat_rotate(turn, (const float[3]){0.0F, 0.0F, -1.0F}, ahead);
    *eye = (vec3f_t){position[0], position[1], position[2]};
    *forward = (vec3f_t){ahead[0], ahead[1], ahead[2]};
}

camera_t
r3d_scene_camera_at(const r3d_scene_camera_t* camera, uint32_t t_ms) {
    camera_t out = {.half_fov_short_tan = camera->half_fov_short_tan, .near_z = camera->near_z};
    r3d_scene_camera_sample(camera, t_ms, &out.eye, &out.forward);
    return out;
}

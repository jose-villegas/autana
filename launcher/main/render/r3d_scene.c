#include "render/r3d_scene.h"

#include <math.h>
#include <string.h>

#pragma GCC diagnostic error "-Wdouble-promotion"

bool
r3d_transform_is_identity(const r3d_transform_t* transform) {
    return transform->position.x == 0.0F && transform->position.y == 0.0F && transform->position.z == 0.0F
           && transform->rotation.x == 0.0F && transform->rotation.y == 0.0F && transform->rotation.z == 0.0F
           && transform->scale.x == 1.0F && transform->scale.y == 1.0F && transform->scale.z == 1.0F;
}

const r3d_scene_renderer_t*
r3d_scene_find_renderer(const r3d_scene_t* scene, const char* name) {
    for (int i = 0; i < scene->renderer_count; i++) {
        if (strcmp(scene->renderers[i].name, name) == 0) {
            return &scene->renderers[i];
        }
    }
    return NULL;
}

uint32_t
r3d_scene_camera_period_ms(const r3d_scene_camera_t* camera) {
    return camera->path == NULL ? 0 : camera->path->clip->duration_ms;
}

/* The direction a glTF camera, or an object, looks: its own -Z, turned. */
static vec3f_t
forward_of(const r3d_transform_t* transform) {
    const float to_radians = 0.017453292519943295F;
    const float pitch = transform->rotation.x * to_radians;
    const float yaw = transform->rotation.y * to_radians;
    /* Ry(yaw) * Rx(pitch) * (0, 0, -1), roll leaving it where it was. */
    return (vec3f_t){sinf(yaw) * -cosf(pitch), sinf(pitch), -cosf(yaw) * cosf(pitch)};
}

void
r3d_scene_camera_sample(const r3d_scene_camera_t* camera, uint32_t t_ms, vec3f_t* eye, vec3f_t* forward) {
    if (camera->path == NULL) {
        *eye = camera->transform.position;
        *forward = forward_of(&camera->transform);
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

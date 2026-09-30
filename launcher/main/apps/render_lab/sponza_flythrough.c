#include "sponza_flythrough.h"

#include "anim/anim_track.h"
#include "flythrough_tracks_generated.h"

uint32_t
sponza_flythrough_period_ms(void) {
    return flythrough_clip.duration_ms;
}

void
sponza_flythrough_sample(uint32_t t_ms, r3d_vec3f_t* eye, r3d_vec3f_t* forward) {
    float position[ANIM_WIDTH_MAX];
    float turn[ANIM_WIDTH_MAX];
    float ahead[3];
    const float seconds = anim_clip_seconds(&flythrough_clip, t_ms, ANIM_LOOP);
    anim_track_sample(&flythrough_camera_translation, seconds, position);
    anim_track_sample(&flythrough_camera_rotation, seconds, turn);
    /* A glTF camera looks down its own -Z. */
    anim_quat_rotate(turn, (const float[3]){0.0F, 0.0F, -1.0F}, ahead);
    *eye = (r3d_vec3f_t){position[0], position[1], position[2]};
    *forward = (r3d_vec3f_t){ahead[0], ahead[1], ahead[2]};
}

r3d_camera_t
sponza_camera_at(uint32_t t_ms) {
    r3d_camera_t camera = {.half_fov_short_tan = SPONZA_HALF_FOV_SHORT_TAN, .near_z = SPONZA_NEAR_Z};
    sponza_flythrough_sample(t_ms, &camera.eye, &camera.forward);
    return camera;
}

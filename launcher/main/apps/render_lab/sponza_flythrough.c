#include "sponza_flythrough.h"

#include "sponza_scene_generated.h"

uint32_t
sponza_flythrough_period_ms(void) {
    return r3d_scene_camera_period_ms(&sponza_scene_camera);
}

void
sponza_flythrough_sample(uint32_t t_ms, vec3_t* eye, vec3_t* forward) {
    r3d_scene_camera_sample(&sponza_scene_camera, t_ms, eye, forward);
}

camera_t
sponza_camera_at(uint32_t t_ms) {
    return r3d_scene_camera_at(&sponza_scene_camera, t_ms);
}

#include "sponza_flythrough.h"

#include "asset/asset_store.h"
#include "sponza_scene_generated.h"

uint32_t
sponza_flythrough_period_ms(void) {
    return r3d_scene_camera_period_ms(&sponza_scene_camera);
}

void
sponza_flythrough_sample(uint32_t t_ms, vec3f_t* eye, vec3f_t* forward) {
    r3d_scene_camera_sample(&sponza_scene_camera, t_ms, eye, forward);
}

camera_t
sponza_camera_at(uint32_t t_ms) {
    return r3d_scene_camera_at(&sponza_scene_camera, t_ms);
}

asset_status_t
sponza_open_meshes(const char** failed) {
    return r3d_scene_bind(asset_store_pack(), &sponza_scene_assets, failed);
}

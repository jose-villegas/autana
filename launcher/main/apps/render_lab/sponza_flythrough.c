#include "sponza_flythrough.h"

#include <stddef.h>

#include "sponza_scene_generated.h"

/* The scene's camera as a bare lens and path, for sampling without loading it. */
static r3d_scene_camera_t
flythrough_camera(void) {
    const scene_camera_def_t* def = &sponza_scene.cameras[0];
    return (r3d_scene_camera_t){def->half_fov_short_tan, def->near_z, NULL, def->path};
}

uint32_t
sponza_flythrough_period_ms(void) {
    const r3d_scene_camera_t camera = flythrough_camera();
    return r3d_scene_camera_period_ms(&camera);
}

void
sponza_flythrough_sample(uint32_t t_ms, vec3f_t* eye, vec3f_t* forward) {
    const r3d_scene_camera_t camera = flythrough_camera();
    r3d_scene_camera_sample(&camera, t_ms, eye, forward);
}

camera_t
sponza_camera_at(uint32_t t_ms) {
    const r3d_scene_camera_t camera = flythrough_camera();
    return r3d_scene_camera_at(&camera, t_ms);
}

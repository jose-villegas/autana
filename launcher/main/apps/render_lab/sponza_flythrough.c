#include "sponza_flythrough.h"

#include <stddef.h>

#include "sponza_scene_generated.h"

/* The scene's camera lens and path, sampled without loading the scene. */
#define FLYTHROUGH (&sponza_scene.cameras[0].lens)

uint32_t
sponza_flythrough_period_ms(void) {
    return r3d_scene_camera_period_ms(FLYTHROUGH);
}

void
sponza_flythrough_sample(uint32_t t_ms, vec3f_t* eye, vec3f_t* forward) {
    r3d_scene_camera_sample(FLYTHROUGH, t_ms, eye, forward);
}

camera_t
sponza_camera_at(uint32_t t_ms) {
    return r3d_scene_camera_at(FLYTHROUGH, t_ms);
}

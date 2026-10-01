#include "sponza_flythrough.h"

#include <stdbool.h>
#include <stddef.h>

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
    /* Checking the three meshes costs about 11 ms on the board, so it is done
     * once and every later scene entry reuses the views. */
    static bool opened;
    if (opened) {
        if (failed != NULL) {
            *failed = NULL;
        }
        return ASSET_OK;
    }
    const asset_status_t status = r3d_scene_bind(asset_store_pack(), &sponza_scene_assets, failed);
    opened = status == ASSET_OK;
    return status;
}

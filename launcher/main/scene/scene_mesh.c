/* scene_mesh: static mesh positions kept in one scene-owned PSRAM block. */
#include <stdalign.h>
#include <string.h>

#include "scene/scene_internal.h"

static bool
mesh_is_first(const scene_t* scene, int index) {
    for (int i = 0; i < index; i++) {
        if (scene->renderers[i].mesh.positions == scene->renderers[index].mesh.positions) {
            return false;
        }
    }
    return true;
}

scene_failure_t
scene_mesh_make_resident(scene_t* scene) {
    size_t total = 0;
    for (int i = 0; i < scene->asset.renderer_count; i++) {
        if (mesh_is_first(scene, i)) {
            const r3d_lit_mesh_t* mesh = &scene->renderers[i].mesh;
            (void)scene_storage_carve(&total, (size_t)mesh->vertex_count * sizeof *mesh->positions,
                                      alignof(int16_t[3]));
        }
    }
    if (total == 0) {
        return scene_failure_make(SCENE_OK, ASSET_OK, scene->id);
    }
    scene->resident = scene_allocate_kept(total);
    if (scene->resident == NULL) {
        return scene_failure_make(SCENE_ERR_MEMORY, ASSET_OK, scene->id);
    }
    size_t end = 0;
    for (int i = scene->asset.renderer_count - 1; i >= 0; i--) {
        if (!mesh_is_first(scene, i)) {
            continue;
        }
        const r3d_lit_mesh_t* mesh = &scene->renderers[i].mesh;
        const int16_t (*source)[3] = mesh->positions;
        const size_t bytes = (size_t)mesh->vertex_count * sizeof *source;
        const size_t at = scene_storage_carve(&end, bytes, alignof(int16_t[3]));
        void* positions = (uint8_t*)scene->resident + at;
        memcpy(positions, source, bytes);
        for (int j = i; j < scene->asset.renderer_count; j++) {
            if (scene->renderers[j].mesh.positions == source) {
                scene->renderers[j].mesh.positions = positions;
            }
        }
    }
    return scene_failure_make(SCENE_OK, ASSET_OK, scene->id);
}

/* scene_mesh: static mesh arrays (positions, triangles, colours) kept in one scene-owned PSRAM block. */
#include <stdalign.h>
#include <stddef.h>
#include <string.h>

#include "scene/scene_internal.h"

static const struct {
    size_t offset, size, alignment;
    bool per_vertex;
} mesh_arrays[] = {
    {offsetof(r3d_lit_mesh_t, positions), sizeof(int16_t[3]), alignof(int16_t[3]), true},
    {offsetof(r3d_lit_mesh_t, triangles), sizeof(uint16_t[3]), alignof(uint16_t[3]), false},
    {offsetof(r3d_lit_mesh_t, colors), sizeof(uint8_t[3]), alignof(uint8_t[3]), true},
    {offsetof(r3d_lit_mesh_t, face_colors), sizeof(uint16_t), alignof(uint16_t), false},
};

static const void*
mesh_array(const r3d_lit_mesh_t* mesh, size_t offset) {
    const void* source;
    memcpy(&source, (const uint8_t*)mesh + offset, sizeof source);
    return source;
}

static bool
mesh_is_first(const scene_t* scene, int index, size_t offset) {
    for (int i = 0; i < index; i++) {
        if (mesh_array(&scene->renderers[i].mesh, offset) == mesh_array(&scene->renderers[index].mesh, offset)) {
            return false;
        }
    }
    return true;
}

scene_failure_t
scene_mesh_make_resident(scene_t* scene) {
    size_t total = 0;
    for (size_t a = 0; a < sizeof mesh_arrays / sizeof mesh_arrays[0]; a++) {
        for (int i = scene->asset.renderer_count - 1; i >= 0; i--) {
            const r3d_lit_mesh_t* mesh = &scene->renderers[i].mesh;
            if (mesh_array(mesh, mesh_arrays[a].offset) != NULL && mesh_is_first(scene, i, mesh_arrays[a].offset)) {
                const int count = mesh_arrays[a].per_vertex ? mesh->vertex_count : mesh->triangle_count;
                (void)scene_storage_carve(&total, (size_t)count * mesh_arrays[a].size, mesh_arrays[a].alignment);
            }
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
    for (size_t a = 0; a < sizeof mesh_arrays / sizeof mesh_arrays[0]; a++) {
        for (int i = scene->asset.renderer_count - 1; i >= 0; i--) {
            const r3d_lit_mesh_t* mesh = &scene->renderers[i].mesh;
            const void* source = mesh_array(mesh, mesh_arrays[a].offset);
            if (source == NULL || !mesh_is_first(scene, i, mesh_arrays[a].offset)) {
                continue;
            }
            const int count = mesh_arrays[a].per_vertex ? mesh->vertex_count : mesh->triangle_count;
            const size_t bytes = (size_t)count * mesh_arrays[a].size;
            const size_t at = scene_storage_carve(&end, bytes, mesh_arrays[a].alignment);
            void* copy = (uint8_t*)scene->resident + at;
            memcpy(copy, source, bytes);
            for (int j = i; j < scene->asset.renderer_count; j++) {
                r3d_lit_mesh_t* target = &scene->renderers[j].mesh;
                if (mesh_array(target, mesh_arrays[a].offset) == source) {
                    memcpy((uint8_t*)target + mesh_arrays[a].offset, &copy, sizeof copy);
                }
            }
        }
    }
    return scene_failure_make(SCENE_OK, ASSET_OK, scene->id);
}

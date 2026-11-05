/* scene_mesh: static mesh arrays (positions, triangles, colours) kept in one scene-owned PSRAM block. */
#include <assert.h>
#include <stddef.h>
#include <string.h>

#include "scene/scene_internal.h"

#define MESH_ARRAY(member, per_vertex)                                                                                 \
    {offsetof(r3d_lit_mesh_t, member), sizeof *((r3d_lit_mesh_t*)0)->member,                                           \
     _Alignof(__typeof__(*((r3d_lit_mesh_t*)0)->member)), per_vertex}

/* One table keeps sizing, copying and repointing in agreement about which arrays exist. */
static const struct {
    size_t offset, size, alignment;
    bool per_vertex;
} mesh_arrays[] = {MESH_ARRAY(positions, true), MESH_ARRAY(triangles, false), MESH_ARRAY(colors, true),
                   MESH_ARRAY(face_colors, false)};

/* Offsets and memcpy avoid aliasing the differently typed pointer-to-const members. */
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

static int
mesh_array_count(const r3d_lit_mesh_t* mesh, size_t array) {
    return mesh_arrays[array].per_vertex ? mesh->vertex_count : mesh->triangle_count;
}

static void
mesh_array_repoint(scene_t* scene, int index, size_t array, const void* source, void* copy) {
    for (int j = index; j < scene->asset.renderer_count; j++) {
        r3d_lit_mesh_t* target = &scene->renderers[j].mesh;
        if (mesh_array(target, mesh_arrays[array].offset) == source) {
            assert(mesh_array_count(target, array) == mesh_array_count(&scene->renderers[index].mesh, array));
            memcpy((uint8_t*)target + mesh_arrays[array].offset, &copy, sizeof copy);
        }
    }
}

static size_t
resident_walk(scene_t* scene, void* block) {
    size_t end = 0;
    for (size_t a = 0; a < sizeof mesh_arrays / sizeof mesh_arrays[0]; a++) {
        for (int i = scene->asset.renderer_count - 1; i >= 0; i--) {
            const r3d_lit_mesh_t* mesh = &scene->renderers[i].mesh;
            const void* source = mesh_array(mesh, mesh_arrays[a].offset);
            if (source == NULL || !mesh_is_first(scene, i, mesh_arrays[a].offset)) {
                continue;
            }
            const size_t bytes = (size_t)mesh_array_count(mesh, a) * mesh_arrays[a].size;
            const size_t at = scene_storage_carve(&end, bytes, mesh_arrays[a].alignment);
            if (block == NULL) {
                continue;
            }
            void* copy = (uint8_t*)block + at;
            memcpy(copy, source, bytes);
            mesh_array_repoint(scene, i, a, source, copy);
        }
    }
    return end;
}

scene_failure_t
scene_mesh_make_resident(scene_t* scene) {
    const size_t total = resident_walk(scene, NULL);
    if (total == 0) {
        return scene_failure_make(SCENE_OK, ASSET_OK, scene->id);
    }
    scene->resident = scene_allocate_kept(total);
    if (scene->resident == NULL) {
        return scene_failure_make(SCENE_ERR_MEMORY, ASSET_OK, scene->id);
    }
    (void)resident_walk(scene, scene->resident);
    return scene_failure_make(SCENE_OK, ASSET_OK, scene->id);
}

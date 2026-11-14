/* scene_mesh: static mesh arrays (positions, triangles, colours) and the
 * batches' copy places kept in one scene-owned PSRAM block. */
#include <assert.h>
#include <stddef.h>
#include <string.h>

#include "scene/scene_internal.h"

#define MESH_ARRAY(member, per_vertex, colours)                                                                        \
    {offsetof(r3d_lit_mesh_t, member), sizeof *((r3d_lit_mesh_t*)0)->member,                                           \
     _Alignof(__typeof__(*((r3d_lit_mesh_t*)0)->member)), per_vertex, colours}

/* One table keeps sizing, copying and repointing in agreement about which arrays exist. */
static const struct {
    size_t offset, size, alignment;
    bool per_vertex, colours; /* colours: a batch's copies' sets follow the one opened */
} mesh_arrays[] = {MESH_ARRAY(positions, true, false), MESH_ARRAY(triangles, false, false),
                   MESH_ARRAY(colors, true, true), MESH_ARRAY(face_colors, false, true)};

/* Offsets and memcpy avoid aliasing the differently typed pointer-to-const members. */
static const void*
mesh_array(const r3d_lit_mesh_t* mesh, size_t offset) {
    const void* source;
    memcpy(&source, (const uint8_t*)mesh + offset, sizeof source);
    return source;
}

/* The elements of `array` renderer `r` draws from: a batch's colours run
 * through every copy's set. */
static size_t
mesh_array_count(const scene_renderer_t* r, size_t array) {
    const int one = mesh_arrays[array].per_vertex ? r->mesh.vertex_count : r->mesh.triangle_count;
    return (size_t)one * (mesh_arrays[array].colours ? r->colour_sets : 1U);
}

/* Whether renderer `r` draws `count` elements of `array` from `source`. */
static bool
mesh_array_is(const scene_renderer_t* r, size_t array, const void* source, size_t count) {
    return mesh_array(&r->mesh, mesh_arrays[array].offset) == source && mesh_array_count(r, array) == count;
}

static bool
mesh_is_first(const scene_t* scene, int index, size_t array) {
    const scene_renderer_t* r = &scene->renderers[index];
    const void* source = mesh_array(&r->mesh, mesh_arrays[array].offset);
    for (int i = 0; i < index; i++) {
        if (mesh_array_is(&scene->renderers[i], array, source, mesh_array_count(r, array))) {
            return false;
        }
    }
    return true;
}

static void
mesh_array_repoint(scene_t* scene, int index, size_t array, const void* source, void* copy) {
    const size_t count = mesh_array_count(&scene->renderers[index], array);
    for (int j = index; j < scene->asset.renderer_count; j++) {
        scene_renderer_t* target = &scene->renderers[j];
        if (mesh_array_is(target, array, source, count)) {
            memcpy((uint8_t*)&target->mesh + mesh_arrays[array].offset, &copy, sizeof copy);
        }
    }
}

/* Carves `bytes` for a copy of `source` and, when `block` is given, copies
 * it there and tells it into `told`, when given; NULL while measuring. */
static void*
copy_in(void* block, size_t* end, size_t alignment, const void* source, size_t bytes, asset_copy_t* told, int* count) {
    const size_t at = scene_storage_carve(end, bytes, alignment);
    const int copied = (*count)++;
    if (block == NULL) {
        return NULL;
    }
    void* copy = (uint8_t*)block + at;
    memcpy(copy, source, bytes);
    if (told != NULL) {
        told[copied] = (asset_copy_t){copy, source, bytes};
    }
    return copy;
}

/* Lays the copies out from offset 0, copying into `block` when given and
 * telling each copy's source into `told`, when given; `*count` copies. */
static size_t
resident_walk(scene_t* scene, void* block, asset_copy_t* told, int* count) {
    size_t end = 0;
    *count = 0;
    for (size_t a = 0; a < sizeof mesh_arrays / sizeof mesh_arrays[0]; a++) {
        for (int i = scene->asset.renderer_count - 1; i >= 0; i--) {
            const r3d_lit_mesh_t* mesh = &scene->renderers[i].mesh;
            const void* source = mesh_array(mesh, mesh_arrays[a].offset);
            if (source == NULL || !mesh_is_first(scene, i, a)) {
                continue;
            }
            const size_t bytes = mesh_array_count(&scene->renderers[i], a) * mesh_arrays[a].size;
            void* copy = copy_in(block, &end, mesh_arrays[a].alignment, source, bytes, told, count);
            if (copy != NULL) {
                mesh_array_repoint(scene, i, a, source, copy);
            }
        }
    }
    /* What the cull reads of every copy. */
    for (int i = 0; i < scene->asset.batch_count; i++) {
        r3d_batch_t* batch = &scene->batches[i];
        const size_t bytes = sizeof *batch->places * batch->copy_count;
        void* copy = copy_in(block, &end, _Alignof(r3d_copy_place_t), batch->places, bytes, told, count);
        if (copy != NULL) {
            batch->places = copy;
        }
    }
    return end;
}

scene_failure_t
scene_mesh_make_resident(scene_t* scene) {
    int count;
    const size_t total = resident_walk(scene, NULL, NULL, &count);
    if (total == 0) {
        return scene_failure_make(SCENE_OK, ASSET_OK, scene->root->id);
    }
    asset_copy_t* told = NULL;
#if BUILD_VARIANT_DEVELOPMENT_OR_HOST
    /* The copies' sources, so a frame record names a resident mesh by the
     * pack entry it copies; without the memory, it names none. Taken first,
     * so the copies themselves are the scene's last take. */
    scene->copies = scene_allocate_kept_optional(sizeof *scene->copies + (sizeof *told * (size_t)count));
    if (scene->copies != NULL) {
        told = (asset_copy_t*)(void*)(scene->copies + 1);
        *scene->copies = (asset_copies_t){.copies = told, .count = count};
    }
#endif
    scene->resident = scene_allocate_kept(total);
    if (scene->resident == NULL) {
        return scene_failure_make(SCENE_ERR_MEMORY, ASSET_OK, scene->root->id);
    }
    (void)resident_walk(scene, scene->resident, told, &count);
#if BUILD_VARIANT_DEVELOPMENT_OR_HOST
    if (scene->copies != NULL) {
        asset_store_copies_add(scene->copies);
    }
#endif
    return scene_failure_make(SCENE_OK, ASSET_OK, scene->root->id);
}

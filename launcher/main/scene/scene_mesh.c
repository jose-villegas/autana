/* scene_mesh: static mesh arrays (positions, triangles, colours) and the
 * batches' copy places, copied into scene-owned PSRAM when a renderer is
 * first drawn: one take per renderer, an array it shares copied once. */
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

#define MESH_ARRAY_COUNT (sizeof mesh_arrays / sizeof mesh_arrays[0])
_Static_assert(MESH_ARRAY_COUNT <= 8, "a renderer's resident bits fit its byte");

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

/* Points every renderer that draws `array` from `source` and is not yet
 * resident in it at `copy`, so a shared array is copied once. */
static void
mesh_array_repoint(scene_t* scene, const scene_renderer_t* r, size_t array, const void* source, void* copy) {
    const size_t count = mesh_array_count(r, array);
    for (int j = 0; j < scene->asset.renderer_count; j++) {
        scene_renderer_t* target = &scene->renderers[j];
        if ((target->resident_arrays & (1U << array)) == 0 && mesh_array_is(target, array, source, count)) {
            memcpy((uint8_t*)&target->mesh + mesh_arrays[array].offset, &copy, sizeof copy);
            target->resident_arrays |= (uint8_t)(1U << array);
        }
    }
}

/* Carves `bytes` for a copy of `source` and, when `block` is given, copies
 * it there and tells it into `told`, when given; NULL while measuring. */
static void*
copy_in(void* block, size_t* end, size_t alignment, const void* source, size_t bytes, asset_copy_t* told, int* count) {
    const size_t at = mem_carve(end, bytes, alignment);
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

/* Lays renderer `index`'s arrays not yet resident, and its batch's copy
 * places, out from offset 0, copying into `block` when given and telling
 * each copy's source into `told`, when given; `*count` copies. */
static size_t
resident_walk(scene_t* scene, int index, void* block, asset_copy_t* told, int* count) {
    scene_renderer_t* r = &scene->renderers[index];
    size_t end = 0;
    *count = 0;
    for (size_t a = 0; a < MESH_ARRAY_COUNT; a++) {
        const void* source = mesh_array(&r->mesh, mesh_arrays[a].offset);
        if (source == NULL || (r->resident_arrays & (1U << a)) != 0) {
            continue;
        }
        const size_t bytes = mesh_array_count(r, a) * mesh_arrays[a].size;
        void* copy = copy_in(block, &end, mesh_arrays[a].alignment, source, bytes, told, count);
        if (copy != NULL) {
            mesh_array_repoint(scene, r, a, source, copy);
        }
    }
    /* What the cull reads of every copy: its renderer's own batch. */
    if (r->batch != NULL) {
        r3d_batch_t* batch = r->batch;
        const size_t bytes = sizeof *batch->places * batch->copy_count;
        void* copy = copy_in(block, &end, _Alignof(r3d_copy_place_t), batch->places, bytes, told, count);
        if (copy != NULL) {
            batch->places = copy;
        }
    }
    return end;
}

bool
scene_mesh_take(scene_t* scene, int index) {
    scene_renderer_t* r = &scene->renderers[index];
    int count;
    const size_t total = resident_walk(scene, index, NULL, NULL, &count);
    if (total == 0) {
        r->resident_arrays = SCENE_RESIDENT_ALL;
        return true;
    }
    /* Taken first, so a refusal leaves nothing taken. */
    r->resident = scene_allocate_kept(total);
    if (r->resident == NULL) {
        return false;
    }
    asset_copy_t* told = NULL;
#if BUILD_VARIANT_DEVELOPMENT_OR_HOST
    /* The copies' sources, so a frame record names a resident mesh by the
     * pack entry it copies; without the memory, it names none. */
    r->copies = scene_allocate_kept_optional(sizeof *r->copies + (sizeof *told * (size_t)count));
    if (r->copies != NULL) {
        told = (asset_copy_t*)(void*)(r->copies + 1);
        *r->copies = (asset_copies_t){.copies = told, .count = count};
    }
#endif
    (void)resident_walk(scene, index, r->resident, told, &count);
    r->resident_arrays = SCENE_RESIDENT_ALL;
#if BUILD_VARIANT_DEVELOPMENT_OR_HOST
    if (r->copies != NULL) {
        asset_store_copies_add(r->copies);
    }
#endif
    return true;
}

void
scene_mesh_release(scene_renderer_t* r) {
#if BUILD_VARIANT_DEVELOPMENT_OR_HOST
    if (r->copies != NULL) {
        asset_store_copies_remove(r->copies);
        scene_free_kept(r->copies);
        r->copies = NULL;
    }
#endif
    scene_free_kept(r->resident);
    r->resident = NULL;
}

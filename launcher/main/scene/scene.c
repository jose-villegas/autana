#include "scene/scene.h"

#include <assert.h>
#include <stdalign.h>
#include <stdlib.h>
#include <string.h>

#include "asset/asset_store.h"
#include "gfx/gfx_color.h"
#include "scene/scene_internal.h"
#include "util/runtime/memory.h"

#define LOADED_MAX 8

#if !defined(ESP_PLATFORM) || CONFIG_LAUNCHER_DEVELOPMENT
bool scene_fail_next_allocation;
#endif
static scene_t* loaded[LOADED_MAX];
static int loaded_count;

static const r3d_placement_t IDENTITY = {{{1.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F}, {0.0F, 0.0F, 1.0F}},
                                         {0.0F, 0.0F, 0.0F}};

/* The block's layout: the header, then each array on its own alignment. */
typedef struct {
    size_t renderers, cameras, instances, transforms, flags, total;
} layout_t;

static size_t
carve(size_t* end, size_t bytes, size_t align) {
    const size_t at = (*end + align - 1) & ~(align - 1);
    *end = at + bytes;
    return at;
}

static layout_t
layout_for(const scene_asset_t* asset) {
    layout_t l;
    size_t end = sizeof(scene_t);
    l.renderers = carve(&end, sizeof(scene_renderer_t) * asset->renderer_count, alignof(scene_renderer_t));
    l.cameras = carve(&end, sizeof(scene_camera_t) * asset->camera_count, alignof(scene_camera_t));
    l.instances = carve(&end, sizeof(r3d_instance_t) * asset->renderer_count, alignof(r3d_instance_t));
    l.transforms = carve(&end, sizeof(scene_transform_t) * asset->entity_count, alignof(scene_transform_t));
    l.flags = carve(&end, asset->entity_count, 1);
    l.total = end;
    return l;
}

static scene_t*
instantiate(const scene_asset_t* asset) {
    const layout_t l = layout_for(asset);
#if !defined(ESP_PLATFORM) || CONFIG_LAUNCHER_DEVELOPMENT
    if (scene_fail_next_allocation) {
        scene_fail_next_allocation = false;
        return NULL;
    }
#endif
    uint8_t* block = memory_alloc(l.total, MEMORY_PSRAM);
    if (block == NULL) {
        return NULL;
    }
    memset(block, 0, l.total);
    scene_t* scene = (scene_t*)(void*)block;
    scene->asset = *asset;
    scene->renderers = (scene_renderer_t*)(void*)(block + l.renderers);
    scene->cameras = (scene_camera_t*)(void*)(block + l.cameras);
    scene->instances = (r3d_instance_t*)(void*)(block + l.instances);
    scene->transforms = (scene_transform_t*)(void*)(block + l.transforms);
    scene->flags = block + l.flags;
    memcpy(scene->transforms, asset->transforms, sizeof(scene_transform_t) * asset->entity_count);
    memset(scene->flags, SCENE_FLAG_ENABLED | SCENE_FLAG_DIRTY, asset->entity_count);
    for (int i = 0; i < asset->camera_count; i++) {
        const scene_asset_camera_t* c = &asset->cameras[i];
        scene->cameras[i] = (scene_camera_t){.lens = {.half_fov_short_tan = c->half_fov_short_tan,
                                                      .near_z = c->near_z,
                                                      .placement = &scene->transforms[c->entity],
                                                      .path = NULL},
                                             .entity = c->entity,
                                             .render_scale_percent = 50,
                                             .clear = GFX_RGB(c->clear_rgb)};
    }
    for (int i = 0; i < asset->renderer_count; i++) {
        scene->renderers[i].entity = asset->renderers[i].entity;
    }
    return scene;
}

/* `id`, cut to what a pack name holds: an id from the pack always fits, a
 * caller's may not. */
static void
copy_id(char out[ASSET_NAME_MAX], const char* id) {
    size_t length = strlen(id);
    if (length >= ASSET_NAME_MAX) {
        length = ASSET_NAME_MAX - 1U;
    }
    (void)memcpy(out, id, length);
    out[length] = '\0';
}

static scene_failure_t
failure(scene_status_t status, asset_status_t asset, const char* what) {
    scene_failure_t out = {status, asset, ""};
    copy_id(out.what, what);
    return out;
}

/* Opens every renderer's mesh; stops at the first that fails. */
static scene_failure_t
open_meshes(scene_t* scene, const asset_pack_t* pack) {
    for (int i = 0; i < scene->asset.renderer_count; i++) {
        const char* mesh = scene->asset.renderers[i].mesh;
        const asset_status_t status = r3d_lit_mesh_open(pack, mesh, &scene->renderers[i].mesh);
        if (status != ASSET_OK) {
            return failure(SCENE_ERR_ASSET, status, mesh);
        }
    }
    return failure(SCENE_OK, ASSET_OK, scene->id);
}

/* Points each camera that has a clip at its node's translation and rotation. */
static scene_failure_t
open_paths(scene_t* scene, const asset_pack_t* pack) {
    for (int i = 0; i < scene->asset.camera_count; i++) {
        const scene_asset_camera_t* c = &scene->asset.cameras[i];
        if (c->clip[0] == '\0') {
            continue;
        }
        scene_camera_t* camera = &scene->cameras[i];
        anim_tracks_t tracks;
        anim_node_tracks_t node;
        asset_status_t status = anim_tracks_from_pack(pack, c->clip, &tracks);
        if (status == ASSET_OK) {
            status = anim_tracks_find_node(&tracks, c->node, &node);
        }
        if (status != ASSET_OK) {
            return failure(SCENE_ERR_ASSET, status, c->clip);
        }
        camera->path.translation = node.translation;
        camera->path.rotation = node.rotation;
        camera->path.clip = tracks.clip;
        camera->lens.path = &camera->path;
    }
    return failure(SCENE_OK, ASSET_OK, scene->id);
}

/* The pack's scene entry `id`, opened. */
static scene_failure_t
open_asset(const asset_pack_t* pack, const char* id, scene_asset_t* asset) {
    if (pack == NULL) {
        return failure(SCENE_ERR_ASSET, ASSET_ERR_NO_PACK, id);
    }
    asset_view_t entry;
    asset_status_t status = asset_pack_find(pack, id, SCENE_ASSET, &entry);
    if (status == ASSET_ERR_NOT_FOUND) {
        return failure(SCENE_ERR_UNKNOWN, ASSET_OK, id);
    }
    if (status == ASSET_OK) {
        status = scene_asset_open(entry, asset);
    }
    return failure(status == ASSET_OK ? SCENE_OK : SCENE_ERR_ASSET, status, id);
}

static scene_t*
fail(scene_failure_t* why, scene_failure_t what) {
    if (why != NULL) {
        *why = what;
    }
    return NULL;
}

scene_t*
scene_load_from(const asset_pack_t* pack, const char* id, scene_failure_t* why) {
    scene_asset_t asset;
    const scene_failure_t opened = open_asset(pack, id, &asset);
    if (opened.status != SCENE_OK) {
        return fail(why, opened);
    }
    if (loaded_count == LOADED_MAX) {
        return fail(why, failure(SCENE_ERR_FULL, ASSET_OK, id));
    }
    scene_t* scene = instantiate(&asset);
    if (scene == NULL) {
        return fail(why, failure(SCENE_ERR_MEMORY, ASSET_OK, id));
    }
    copy_id(scene->id, id);
    scene_failure_t done = open_meshes(scene, pack);
    if (done.status == SCENE_OK) {
        done = open_paths(scene, pack);
    }
    if (why != NULL) {
        *why = done;
    }
    if (done.status != SCENE_OK) {
        memory_free(scene);
        return NULL;
    }
    loaded[loaded_count++] = scene;
    return scene;
}

scene_t*
scene_load(const char* id, scene_failure_t* why) {
    const asset_pack_t* pack = asset_store_pack(id);
    scene_t* scene = scene_load_from(pack, id, why);
    if (scene != NULL) {
        scene->holds_pack = true;
    } else if (pack != NULL) {
        asset_store_release(id);
    }
    return scene;
}

void
scene_unload(scene_t* scene) {
    if (scene == NULL) {
        return;
    }
    for (int i = 0; i < loaded_count; i++) {
        if (loaded[i] != scene) {
            continue;
        }
        scene_draw_forget(scene);
        if (scene->holds_pack) {
            asset_store_release(scene->id);
        }
        loaded_count--;
        memmove(&loaded[i], &loaded[i + 1], sizeof loaded[0] * (size_t)(loaded_count - i));
        loaded[loaded_count] = NULL;
        memory_free(scene);
        return;
    }
}

void
scene_unload_all(void) {
    while (loaded_count > 0) {
        scene_unload(loaded[loaded_count - 1]);
    }
    scene_draw_release();
}

int
scene_loaded_count(void) {
    return loaded_count;
}

scene_t*
scene_loaded_at(int index) {
    return loaded[index];
}

scene_entity_t
scene_find(const scene_t* scene, const char* name) {
    for (int i = 0; i < scene->asset.entity_count; i++) {
        if (strcmp(scene->asset.names[i], name) == 0) {
            return (scene_entity_t)i;
        }
    }
    return SCENE_ENTITY_NONE;
}

const char*
scene_entity_mesh_id(const scene_t* scene, scene_entity_t entity) {
    assert(entity < scene->asset.entity_count);
    for (int i = 0; i < scene->asset.renderer_count; i++) {
        if (scene->asset.renderers[i].entity == entity) {
            return scene->asset.renderers[i].mesh;
        }
    }
    return NULL;
}

int
scene_camera_index(const scene_t* scene, const char* camera) {
    for (int i = 0; i < scene->asset.camera_count; i++) {
        if (camera == NULL || strcmp(scene->asset.names[scene->cameras[i].entity], camera) == 0) {
            return i;
        }
    }
    return -1;
}

const r3d_scene_camera_t*
scene_camera_lens(const scene_t* scene, const char* camera) {
    const int index = scene_camera_index(scene, camera);
    return index < 0 ? NULL : &scene->cameras[index].lens;
}

const scene_transform_t*
scene_entity_transform(const scene_t* scene, scene_entity_t entity) {
    assert(entity < scene->asset.entity_count);
    return &scene->transforms[entity];
}

void
scene_entity_set_transform(scene_t* scene, scene_entity_t entity, const scene_transform_t* transform) {
    assert(entity < scene->asset.entity_count);
    scene->transforms[entity] = *transform;
    scene->flags[entity] |= SCENE_FLAG_DIRTY;
}

bool
scene_entity_enabled(const scene_t* scene, scene_entity_t entity) {
    assert(entity < scene->asset.entity_count);
    return (scene->flags[entity] & SCENE_FLAG_ENABLED) != 0;
}

void
scene_entity_set_enabled(scene_t* scene, scene_entity_t entity, bool enabled) {
    assert(entity < scene->asset.entity_count);
    scene->flags[entity] = enabled ? (uint8_t)(scene->flags[entity] | SCENE_FLAG_ENABLED)
                                   : (uint8_t)(scene->flags[entity] & ~SCENE_FLAG_ENABLED);
}

bool
scene_transform_is_identity(const scene_transform_t* transform) {
    return memcmp(transform, &IDENTITY, sizeof IDENTITY) == 0;
}

#include "scene/scene.h"

#include <assert.h>
#include <stdalign.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"

#include "asset/asset_store.h"
#include "scene/scene_internal.h"

#define DEFS_MAX   16
#define LOADED_MAX 8

static const scene_def_t* defs[DEFS_MAX];
static int def_count;
#if !defined(ESP_PLATFORM) || CONFIG_LAUNCHER_DEVELOPMENT
bool scene_fail_next_allocation;
#endif
static scene_t* loaded[LOADED_MAX];
static int loaded_count;

static const r3d_placement_t IDENTITY = {{{1.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F}, {0.0F, 0.0F, 1.0F}},
                                         {0.0F, 0.0F, 0.0F}};

void
scene_register(const scene_def_t* def) {
    assert(def_count < DEFS_MAX);
    defs[def_count++] = def;
}

static const scene_def_t*
find_def(const char* name) {
    for (int i = 0; i < def_count; i++) {
        if (strcmp(defs[i]->name, name) == 0) {
            return defs[i];
        }
    }
    return NULL;
}

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
layout_for(const scene_def_t* def) {
    layout_t l;
    size_t end = sizeof(scene_t);
    l.renderers = carve(&end, sizeof(scene_renderer_t) * def->renderer_count, alignof(scene_renderer_t));
    l.cameras = carve(&end, sizeof(scene_camera_t) * def->camera_count, alignof(scene_camera_t));
    l.instances = carve(&end, sizeof(r3d_instance_t) * def->renderer_count, alignof(r3d_instance_t));
    l.transforms = carve(&end, sizeof(scene_transform_t) * def->entity_count, alignof(scene_transform_t));
    l.flags = carve(&end, def->entity_count, 1);
    l.total = end;
    return l;
}

static scene_t*
instantiate(const scene_def_t* def) {
    const layout_t l = layout_for(def);
#if !defined(ESP_PLATFORM) || CONFIG_LAUNCHER_DEVELOPMENT
    if (scene_fail_next_allocation) {
        scene_fail_next_allocation = false;
        return NULL;
    }
#endif
    uint8_t* block = heap_caps_malloc(l.total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (block == NULL) {
        return NULL;
    }
    memset(block, 0, l.total);
    scene_t* scene = (scene_t*)(void*)block;
    scene->def = def;
    scene->renderers = (scene_renderer_t*)(void*)(block + l.renderers);
    scene->cameras = (scene_camera_t*)(void*)(block + l.cameras);
    scene->instances = (r3d_instance_t*)(void*)(block + l.instances);
    scene->transforms = (scene_transform_t*)(void*)(block + l.transforms);
    scene->flags = block + l.flags;
    memcpy(scene->transforms, def->transforms, sizeof(scene_transform_t) * def->entity_count);
    memset(scene->flags, SCENE_FLAG_ENABLED | SCENE_FLAG_DIRTY, def->entity_count);
    for (int i = 0; i < def->camera_count; i++) {
        const scene_camera_def_t* c = &def->cameras[i];
        scene->cameras[i] = (scene_camera_t){.lens = c->lens, .entity = c->entity, .render_scale_percent = 50};
        scene->cameras[i].lens.placement = &scene->transforms[c->entity];
    }
    for (int i = 0; i < def->renderer_count; i++) {
        scene->renderers[i].entity = def->renderers[i].entity;
    }
    return scene;
}

/* Opens every renderer's mesh; stops at the first that fails. */
static scene_failure_t
open_meshes(scene_t* scene, const asset_pack_t* pack) {
    for (int i = 0; i < scene->def->renderer_count; i++) {
        const char* asset = scene->def->renderers[i].asset;
        const asset_status_t status =
            pack == NULL ? ASSET_ERR_NO_PACK : r3d_lit_mesh_open(pack, asset, &scene->renderers[i].mesh);
        if (status != ASSET_OK) {
            return (scene_failure_t){SCENE_ERR_ASSET, status, asset};
        }
    }
    return (scene_failure_t){SCENE_OK, ASSET_OK, scene->def->name};
}

static scene_t*
fail(scene_failure_t* why, scene_status_t status, const char* what) {
    if (why != NULL) {
        *why = (scene_failure_t){status, ASSET_OK, what};
    }
    return NULL;
}

scene_t*
scene_load_from(const asset_pack_t* pack, const char* name, scene_failure_t* why) {
    const scene_def_t* def = find_def(name);
    if (def == NULL) {
        return fail(why, SCENE_ERR_UNKNOWN, name);
    }
    if (loaded_count == LOADED_MAX) {
        return fail(why, SCENE_ERR_FULL, name);
    }
    scene_t* scene = instantiate(def);
    if (scene == NULL) {
        return fail(why, SCENE_ERR_MEMORY, name);
    }
    const scene_failure_t opened = open_meshes(scene, pack);
    if (why != NULL) {
        *why = opened;
    }
    if (opened.status != SCENE_OK) {
        heap_caps_free(scene);
        return NULL;
    }
    loaded[loaded_count++] = scene;
    return scene;
}

scene_t*
scene_load(const char* name, scene_failure_t* why) {
    return scene_load_from(asset_store_pack(), name, why);
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
        loaded_count--;
        memmove(&loaded[i], &loaded[i + 1], sizeof loaded[0] * (size_t)(loaded_count - i));
        loaded[loaded_count] = NULL;
        heap_caps_free(scene);
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
    for (int i = 0; i < scene->def->entity_count; i++) {
        if (strcmp(scene->def->entity_names[i], name) == 0) {
            return (scene_entity_t)i;
        }
    }
    return SCENE_ENTITY_NONE;
}

const scene_transform_t*
scene_entity_transform(const scene_t* scene, scene_entity_t entity) {
    assert(entity < scene->def->entity_count);
    return &scene->transforms[entity];
}

void
scene_entity_set_transform(scene_t* scene, scene_entity_t entity, const scene_transform_t* transform) {
    assert(entity < scene->def->entity_count);
    scene->transforms[entity] = *transform;
    scene->flags[entity] |= SCENE_FLAG_DIRTY;
}

bool
scene_entity_enabled(const scene_t* scene, scene_entity_t entity) {
    assert(entity < scene->def->entity_count);
    return (scene->flags[entity] & SCENE_FLAG_ENABLED) != 0;
}

void
scene_entity_set_enabled(scene_t* scene, scene_entity_t entity, bool enabled) {
    assert(entity < scene->def->entity_count);
    scene->flags[entity] = enabled ? (uint8_t)(scene->flags[entity] | SCENE_FLAG_ENABLED)
                                   : (uint8_t)(scene->flags[entity] & ~SCENE_FLAG_ENABLED);
}

bool
scene_transform_is_identity(const scene_transform_t* transform) {
    return memcmp(transform, &IDENTITY, sizeof IDENTITY) == 0;
}

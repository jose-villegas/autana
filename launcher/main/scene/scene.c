#include "scene/scene.h"

#include <assert.h>
#include <math.h>
#include <stdalign.h>
#include <stdlib.h>
#include <string.h>

#include "asset/asset_store.h"
#include "core/memory.h"
#include "gfx/draw/gfx_color.h"
#include "math/scalar/mathi.h"
#include "scene/scene_internal.h"

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
    size_t renderers, cameras, instances, flags, locals, animators, skins, lights, total;
} layout_t;

size_t
scene_storage_carve(size_t* end, size_t bytes, size_t align) {
    const size_t at = mathi_size_ceil(*end, align);
    *end = at + bytes;
    return at;
}

static layout_t
layout_for(const scene_asset_t* asset) {
    layout_t l;
    size_t end = sizeof(scene_t);
    SCENE_STORAGE_RESERVE(l, end, renderers, scene_renderer_t, asset->renderer_count);
    SCENE_STORAGE_RESERVE(l, end, cameras, scene_camera_t, asset->camera_count);
    const int entities = asset->entity_count;
    SCENE_STORAGE_RESERVE(l, end, flags, uint8_t, entities);
    SCENE_STORAGE_RESERVE(l, end, locals, transformf_t, entities);
    const int renderers = asset->renderer_count;
    SCENE_STORAGE_RESERVE(l, end, instances, r3d_instance_t, renderers);
    SCENE_STORAGE_RESERVE(l, end, animators, scene_animator_t, entities);
    SCENE_STORAGE_RESERVE(l, end, skins, scene_skinned_renderer_t, entities);
    SCENE_STORAGE_RESERVE(l, end, lights, scene_light_t, entities);
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
    scene->flags = block + l.flags;
    scene->locals = (transformf_t*)(void*)(block + l.locals);
    scene->animators = (scene_animator_t*)(void*)(block + l.animators);
    scene->lights = (scene_light_t*)(void*)(block + l.lights);
    scene->ambient_color = SCENE_LIGHT_COLOR_DEFAULT;
    scene->ambient_intensity = SCENE_AMBIENT_INTENSITY_NONE;
    scene->tonemap_white = SCENE_TONEMAP_WHITE_NONE;
    scene->skinned_renderers = (scene_skinned_renderer_t*)(void*)(block + l.skins);
    memset(scene->flags, SCENE_FLAG_ENABLED | SCENE_FLAG_DIRTY, asset->entity_count);
    for (int i = 0; i < asset->camera_count; i++) {
        const scene_asset_camera_t* c = &asset->cameras[i];
        scene->cameras[i] = (scene_camera_t){.lens = {.half_fov_short_tan = c->half_fov_short_tan,
                                                      .near_z = c->near_z,
                                                      .pose = &scene->locals[c->entity]},
                                             .entity = c->entity,
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

scene_failure_t
scene_failure_make(scene_status_t status, asset_status_t asset, const char* what) {
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
            return scene_failure_make(SCENE_ERR_ASSET, status, mesh);
        }
    }
    return scene_failure_make(SCENE_OK, ASSET_OK, scene->id);
}

static scene_failure_t
open_camera_animators(scene_t* scene) {
    for (int i = 0; i < scene->asset.camera_count; i++) {
        const scene_asset_camera_t* camera = &scene->asset.cameras[i];
        if (camera->clip[0] == '\0') {
            continue;
        }
        const char* clips[] = {camera->clip};
        scene_failure_t why;
        if (!scene_animator_set_clips(scene, camera->entity, NULL, clips, 1, &why)) {
            return why;
        }
        (void)scene_animator_play(scene, camera->entity, 0, 0);
    }
    return scene_failure_make(SCENE_OK, ASSET_OK, scene->id);
}

static bool
local_from_placement(const scene_placement_t* placement, transformf_t* out) {
    mat4f_t matrix = mat4f_identity();
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++) {
            matrix.m[r][c] = placement->m[r][c];
        }
    }
    matrix.m[0][3] = placement->position.x;
    matrix.m[1][3] = placement->position.y;
    matrix.m[2][3] = placement->position.z;
    return transformf_from_mat(matrix, out);
}

static scene_failure_t
open_transforms(scene_t* scene) {
    for (int e = 0; e < scene->asset.entity_count; e++) {
        if (!local_from_placement(&scene->asset.transforms[e], &scene->locals[e])) {
            return scene_failure_make(SCENE_ERR_ASSET, ASSET_ERR_FORMAT, scene->asset.names[e]);
        }
    }
    return scene_failure_make(SCENE_OK, ASSET_OK, scene->id);
}

/* The pack's scene entry `id`, opened. */
static scene_failure_t
open_asset(const asset_pack_t* pack, const char* id, scene_asset_t* asset) {
    if (pack == NULL) {
        return scene_failure_make(SCENE_ERR_ASSET, ASSET_ERR_NO_PACK, id);
    }
    asset_view_t entry;
    asset_status_t status = asset_pack_find(pack, id, SCENE_ASSET, &entry);
    if (status == ASSET_ERR_NOT_FOUND) {
        return scene_failure_make(SCENE_ERR_UNKNOWN, ASSET_OK, id);
    }
    if (status == ASSET_OK) {
        status = scene_asset_open(entry, asset);
    }
    return scene_failure_make(status == ASSET_OK ? SCENE_OK : SCENE_ERR_ASSET, status, id);
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
        return fail(why, scene_failure_make(SCENE_ERR_FULL, ASSET_OK, id));
    }
    scene_t* scene = instantiate(&asset);
    if (scene == NULL) {
        return fail(why, scene_failure_make(SCENE_ERR_MEMORY, ASSET_OK, id));
    }
    scene->pack = pack;
    copy_id(scene->id, id);
    scene_failure_t done = open_meshes(scene, pack);
    if (done.status == SCENE_OK) {
        done = open_transforms(scene);
    }
    if (done.status == SCENE_OK) {
        done = open_camera_animators(scene);
    }
    if (why != NULL) {
        *why = done;
    }
    if (done.status != SCENE_OK) {
        scene_animation_release(scene);
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
        scene_animation_release(scene);
        memory_free(scene->light_transfer);
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

int
scene_entity_count(const scene_t* scene) {
    return scene->asset.entity_count;
}

const char*
scene_entity_name(const scene_t* scene, scene_entity_t entity) {
    assert(entity < scene->asset.entity_count);
    return scene->asset.names[entity];
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

/* The index of the entity's renderer, or -1 when it draws nothing. */
int
scene_renderer_index(const scene_t* scene, scene_entity_t entity) {
    assert(entity < scene->asset.entity_count);
    for (int i = 0; i < scene->asset.renderer_count; i++) {
        if (scene->asset.renderers[i].entity == entity) {
            return i;
        }
    }
    return -1;
}

const char*
scene_entity_mesh_id(const scene_t* scene, scene_entity_t entity) {
    const int i = scene_renderer_index(scene, entity);
    return i < 0 ? NULL : scene->asset.renderers[i].mesh;
}

const r3d_lit_mesh_t*
scene_entity_mesh(const scene_t* scene, scene_entity_t entity) {
    const int i = scene_renderer_index(scene, entity);
    return i < 0 ? NULL : &scene->renderers[i].mesh;
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
    return &scene->locals[entity];
}

void
scene_set_transform(scene_t* scene, scene_entity_t entity, const scene_transform_t* transform) {
    assert(entity < scene->asset.entity_count);
    scene->locals[entity] = *transform;
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
    scene->flags[entity] |= SCENE_FLAG_DIRTY;
    scene->lights_dirty |= scene->lights[entity].kind != SCENE_LIGHT_NONE;
    scene_skinned_renderer_t* skin = &scene->skinned_renderers[entity];
    if (skin->internal_storage != NULL && scene->light_count > 0) {
        const int renderer = scene_renderer_index(scene, entity);
        skin->output.mesh.colors = enabled ? (const uint8_t(*)[3])skin->colors : skin->bind.colors;
        scene->renderers[renderer].mesh.colors = skin->output.mesh.colors;
    }
}

bool
scene_transform_is_identity(const scene_placement_t* transform) {
    return memcmp(transform, &IDENTITY, sizeof IDENTITY) == 0;
}

static scene_light_t*
light_attach(scene_t* scene, scene_entity_t entity) {
    if (scene == NULL || entity >= scene->asset.entity_count) {
        return NULL;
    }
    scene_light_t* light = &scene->lights[entity];
    if (light->kind != SCENE_LIGHT_NONE) {
        return light;
    }
    if (scene->light_count == R3D_SKIN_LIGHT_MAX) {
        return NULL;
    }
    if (scene->light_transfer == NULL) {
#if !defined(ESP_PLATFORM) || CONFIG_LAUNCHER_DEVELOPMENT
        if (scene_fail_next_allocation) {
            scene_fail_next_allocation = false;
            return NULL;
        }
#endif
        scene->light_transfer = memory_alloc(sizeof *scene->light_transfer, MEMORY_INTERNAL);
        if (scene->light_transfer == NULL) {
            return NULL;
        }
        r3d_skin_light_transfer_build(scene->tonemap_white, scene->light_transfer);
        scene->transfer_white = scene->tonemap_white;
    }
    *light = (scene_light_t){SCENE_LIGHT_DIRECTIONAL, SCENE_LIGHT_COLOR_DEFAULT, SCENE_LIGHT_INTENSITY_DEFAULT};
    scene->light_count++;
    for (int r = 0; r < scene->asset.renderer_count; r++) {
        scene_skinned_renderer_t* skin = &scene->skinned_renderers[scene->renderers[r].entity];
        if (skin->internal_storage != NULL && (scene->flags[scene->renderers[r].entity] & SCENE_FLAG_ENABLED)) {
            skin->output.mesh.colors = skin->colors;
            scene->renderers[r].mesh.colors = skin->colors;
        }
    }
    return light;
}

bool
scene_light_set_color(scene_t* scene, scene_entity_t entity, vec3f_t linear_rgb) {
    if (!isfinite(linear_rgb.x) || !isfinite(linear_rgb.y) || !isfinite(linear_rgb.z) || linear_rgb.x < 0.0F
        || linear_rgb.x > 1.0F || linear_rgb.y < 0.0F || linear_rgb.y > 1.0F || linear_rgb.z < 0.0F
        || linear_rgb.z > 1.0F) {
        return false;
    }
    scene_light_t* light = light_attach(scene, entity);
    if (light == NULL) {
        return false;
    }
    light->color = linear_rgb;
    scene->lights_dirty = true;
    return true;
}

bool
scene_light_set_intensity(scene_t* scene, scene_entity_t entity, float intensity) {
    if (!isfinite(intensity) || intensity < 0.0F) {
        return false;
    }
    scene_light_t* light = light_attach(scene, entity);
    if (light == NULL) {
        return false;
    }
    light->intensity = intensity;
    scene->lights_dirty = true;
    return true;
}

vec3f_t
scene_light_color(const scene_t* scene, scene_entity_t entity) {
    return scene == NULL || entity >= scene->asset.entity_count || scene->lights[entity].kind == SCENE_LIGHT_NONE
               ? (vec3f_t){0}
               : scene->lights[entity].color;
}

float
scene_light_intensity(const scene_t* scene, scene_entity_t entity) {
    return scene == NULL || entity >= scene->asset.entity_count || scene->lights[entity].kind == SCENE_LIGHT_NONE
               ? 0.0F
               : scene->lights[entity].intensity;
}

void
scene_lights_prepare(scene_t* scene) {
    if (scene->light_count == 0) {
        return;
    }
    if (scene->transfer_white != scene->tonemap_white) {
        r3d_skin_light_transfer_build(scene->tonemap_white, scene->light_transfer);
        scene->transfer_white = scene->tonemap_white;
    }
    r3d_skin_lights_t world = {.ambient = vec3f_scale(scene->ambient_color, scene->ambient_intensity),
                               .transfer = scene->light_transfer};
    for (int l = 0; l < scene->asset.entity_count; l++) {
        const scene_light_t* light = &scene->lights[l];
        if (light->kind == SCENE_LIGHT_NONE || !(scene->flags[l] & SCENE_FLAG_ENABLED)) {
            continue;
        }
        const int at = world.count++;
        world.direction[at] = quatf_rotate(scene->locals[l].rotation, SCENE_LIGHT_AXIS);
        world.color[at] = vec3f_scale(light->color, light->intensity);
    }
    for (int e = 0; e < scene->asset.entity_count; e++) {
        scene_skinned_renderer_t* skin = &scene->skinned_renderers[e];
        if (skin->internal_storage == NULL || !scene->animators[e].posed) {
            continue;
        }
        const quatf_t rotation = scene->locals[e].rotation;
        const quatf_t inverse = {-rotation.x, -rotation.y, -rotation.z, rotation.w};
        *skin->lights = world;
        for (int l = 0; l < world.count; l++) {
            skin->lights->direction[l] = vec3f_normalize(quatf_rotate(inverse, world.direction[l]));
        }
    }
}

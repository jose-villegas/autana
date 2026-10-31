#include "scene/scene.h"

#include <assert.h>
#include <math.h>
#include <stdalign.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "asset/asset_store.h"
#include "core/memory.h"
#include "gfx/target/gfx_color.h"
#include "image/pixel/rgb565.h"
#include "math/scalar/mathi.h"
#include "render/context/render_context.h"
#include "scene/scene_internal.h"

#define LOADED_MAX 8

#if !defined(ESP_PLATFORM) || CONFIG_LAUNCHER_DEVELOPMENT
int scene_fail_allocation_after = -1;
#endif
static scene_t* loaded[LOADED_MAX];
static int loaded_count;
static memory_take_fn kept_take;

/* The block's layout: the header, then each array on its own alignment. */
typedef struct {
    size_t renderers, cameras, instances, flags, locals, world_slots, animators, skins, lights, total;
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
    SCENE_STORAGE_RESERVE(l, end, renderers, scene_renderer_t, asset->renderer_count + asset->skinned_renderer_count);
    SCENE_STORAGE_RESERVE(l, end, cameras, scene_camera_t, asset->camera_count);
    const int entities = asset->entity_count;
    SCENE_STORAGE_RESERVE(l, end, flags, uint8_t, entities);
    SCENE_STORAGE_RESERVE(l, end, locals, transformf_t, entities);
    SCENE_STORAGE_RESERVE(l, end, world_slots, uint16_t, entities);
    const int renderers = asset->renderer_count + asset->skinned_renderer_count;
    SCENE_STORAGE_RESERVE(l, end, instances, r3d_instance_t, renderers);
    SCENE_STORAGE_RESERVE(l, end, animators, scene_animator_t, entities);
    SCENE_STORAGE_RESERVE(l, end, skins, scene_skinned_renderer_t, entities);
    SCENE_STORAGE_RESERVE(l, end, lights, scene_light_t, entities);
    l.total = end;
    return l;
}

/* True when fault injection fails this allocation. */
static bool
injected_failure(void) {
#if !defined(ESP_PLATFORM) || CONFIG_LAUNCHER_DEVELOPMENT
    if (scene_fail_allocation_after == 0) {
        scene_fail_allocation_after = -1;
        return true;
    }
    if (scene_fail_allocation_after > 0) {
        scene_fail_allocation_after--;
    }
#endif
    return false;
}

void*
scene_allocate(size_t bytes, memory_kind_t kind) {
    return injected_failure() ? NULL : memory_alloc(bytes, kind);
}

void*
scene_allocate_kept(size_t bytes) {
    if (injected_failure()) {
        return NULL;
    }
    return kept_take != NULL ? kept_take(bytes, _Alignof(max_align_t)) : memory_alloc(bytes, MEMORY_PSRAM);
}

void
scene_free_kept(void* block) {
    if (kept_take == NULL) {
        memory_free(block);
    }
}

memory_take_fn
scene_set_take(memory_take_fn take) {
    assert(loaded_count == 0);
    const memory_take_fn replaced = kept_take;
    kept_take = take;
    render_context_set_take(render_context_main(), take);
    return replaced;
}

static scene_t*
instantiate(const scene_asset_t* asset) {
    const layout_t l = layout_for(asset);
    uint8_t* block = scene_allocate_kept(l.total);
    if (block == NULL) {
        return NULL;
    }
    memset(block, 0, l.total);
    scene_t* scene = (scene_t*)(void*)block;
    scene->asset = *asset;
    scene->renderer_count = asset->renderer_count + asset->skinned_renderer_count;
    scene->renderers = (scene_renderer_t*)(void*)(block + l.renderers);
    scene->cameras = (scene_camera_t*)(void*)(block + l.cameras);
    scene->instances = (r3d_instance_t*)(void*)(block + l.instances);
    scene->flags = block + l.flags;
    scene->locals = (transformf_t*)(void*)(block + l.locals);
    scene->world_slots = (uint16_t*)(void*)(block + l.world_slots);
    scene->animators = (scene_animator_t*)(void*)(block + l.animators);
    scene->lights = (scene_light_t*)(void*)(block + l.lights);
    scene->ambient_color = asset->settings->ambient_color;
    scene->ambient_intensity = asset->settings->ambient_intensity;
    scene->tonemap_white = asset->settings->tonemap_white;
    scene->skinned_renderers = (scene_skinned_renderer_t*)(void*)(block + l.skins);
    memset(scene->flags, SCENE_FLAG_ENABLED | SCENE_FLAG_ACTIVE | SCENE_FLAG_CHANGED, asset->entity_count);
    for (int i = 0; i < asset->camera_count; i++) {
        const scene_asset_camera_t* c = &asset->cameras[i];
        scene->cameras[i] = (scene_camera_t){
            .lens = {.half_fov_short_tan = c->half_fov_short_tan, .near_z = c->near_z, .small_fill = SMALL_FILL_SMOOTH},
            .entity = c->entity,
            .clear = RGB565BE(c->clear_rgb)};
    }
    for (int i = 0; i < asset->renderer_count; i++) {
        scene->renderers[i].entity = asset->renderers[i].entity;
    }
    for (int i = 0; i < asset->skinned_renderer_count; i++) {
        scene->renderers[asset->renderer_count + i].entity = asset->skinned_renderers[i].entity;
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
    for (int i = 0; i < scene->renderer_count; i++) {
        const char* mesh = i < scene->asset.renderer_count
                               ? scene->asset.renderers[i].mesh
                               : scene->asset.skinned_renderers[i - scene->asset.renderer_count].mesh;
        r3d_lit_mesh_t* view = &scene->renderers[i].mesh;
        asset_status_t status = r3d_lit_mesh_open(pack, mesh, view);
        if (status == ASSET_OK && i < scene->asset.renderer_count) {
            const uint16_t set = scene->asset.renderers[i].colour_set;
            if (set != SCENE_ASSET_COLOUR_SET_OWN) {
                status = r3d_lit_mesh_open_colours(pack, mesh, set, view);
            }
        }
        if (status == ASSET_OK && view->colors == NULL && view->face_colors == NULL) {
            status = ASSET_ERR_FORMAT;
        }
        if (status != ASSET_OK) {
            return scene_failure_make(SCENE_ERR_ASSET, status, mesh);
        }
    }
    return scene_failure_make(SCENE_OK, ASSET_OK, scene->id);
}

static const char*
skeleton_for(const scene_t* scene, scene_entity_t entity) {
    for (int i = 0; i < scene->asset.skinned_renderer_count; i++) {
        if (scene->asset.skinned_renderers[i].entity == entity) {
            return scene->asset.skinned_renderers[i].skeleton;
        }
    }
    return NULL;
}

static scene_failure_t
open_animator(scene_t* scene, const scene_asset_animator_t* row) {
    if (row->clip_count > ANIM_LAYER_CLIP_NONE) {
        return scene_failure_make(SCENE_ERR_ASSET, ASSET_ERR_BOUNDS, scene->id);
    }
    const char* skeleton = skeleton_for(scene, row->entity);
    if (row->clip_count == 0 && skeleton == NULL) {
        return scene_failure_make(SCENE_OK, ASSET_OK, scene->id);
    }
    const char** clips = NULL;
    if (row->clip_count > 0) {
        clips = scene_allocate(sizeof *clips * row->clip_count, MEMORY_PSRAM);
        if (clips == NULL) {
            return scene_failure_make(SCENE_ERR_MEMORY, ASSET_OK, scene->id);
        }
        for (int c = 0; c < row->clip_count; c++) {
            clips[c] = scene->asset.clips[row->first_clip + c];
        }
    }
    scene_failure_t why;
    const bool bound = scene_animator_set_clips(scene, row->entity, skeleton, clips, row->clip_count, &why);
    memory_free(clips);
    if (!bound) {
        return why;
    }
    scene->animators[row->entity].wrap = (anim_wrap_t)row->wrap;
    if (row->default_clip != SCENE_ASSET_DEFAULT_CLIP_NONE) {
        (void)scene_animator_play(scene, row->entity, row->default_clip, 0);
    }
    return scene_failure_make(SCENE_OK, ASSET_OK, scene->id);
}

static scene_failure_t
open_animators(scene_t* scene) {
    for (int i = 0; i < scene->asset.animator_count; i++) {
        const scene_failure_t why = open_animator(scene, &scene->asset.animators[i]);
        if (why.status != SCENE_OK) {
            return why;
        }
    }
    return scene_failure_make(SCENE_OK, ASSET_OK, scene->id);
}

static scene_failure_t
open_skins(scene_t* scene) {
    for (int i = 0; i < scene->asset.skinned_renderer_count; i++) {
        const scene_asset_skinned_renderer_t* row = &scene->asset.skinned_renderers[i];
        scene_failure_t why;
        if (scene->animators[row->entity].storage == NULL
            && !scene_animator_set_clips(scene, row->entity, row->skeleton, NULL, 0, &why)) {
            return why;
        }
        if (!scene_renderer_set_skin(scene, row->entity, row->skin, &why)) {
            return why;
        }
    }
    return scene_failure_make(SCENE_OK, ASSET_OK, scene->id);
}

static scene_failure_t
open_lights(scene_t* scene) {
    for (int i = 0; i < scene->asset.light_count; i++) {
        const scene_asset_light_t* row = &scene->asset.lights[i];
        if (!scene_light_set_color(scene, row->entity, row->color)
            || !scene_light_set_intensity(scene, row->entity, row->intensity)) {
            return scene_failure_make(SCENE_ERR_MEMORY, ASSET_OK, scene->asset.names[row->entity].name);
        }
    }
    return scene_failure_make(SCENE_OK, ASSET_OK, scene->id);
}

static bool
local_from_row(const scene_asset_transform_t* row, transformf_t* out) {
    mat4f_t matrix = mat4f_identity();
    for (int r = 0; r < MAT3F_AXES; r++) {
        for (int c = 0; c < MAT3F_AXES; c++) {
            matrix.m[r][c] = row->linear.m[r][c];
        }
    }
    matrix.m[0][3] = row->position.x;
    matrix.m[1][3] = row->position.y;
    matrix.m[2][3] = row->position.z;
    return transformf_from_mat(matrix, out);
}

static scene_failure_t
open_transforms(scene_t* scene) {
    for (int e = 0; e < scene->asset.entity_count; e++) {
        /* A parent's even scale keeps every world below it exact. */
        if (!local_from_row(&scene->asset.transforms[e], &scene->locals[e])
            || (scene->world_slots[e] != SCENE_WORLD_SLOT_NONE && !transformf_scale_is_even(&scene->locals[e]))) {
            return scene_failure_make(SCENE_ERR_ASSET, ASSET_ERR_FORMAT, scene->asset.names[e].name);
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
    scene_failure_t done =
        scene_hierarchy_open(scene) ? open_meshes(scene, pack) : scene_failure_make(SCENE_ERR_MEMORY, ASSET_OK, id);
    if (done.status == SCENE_OK) {
        done = open_transforms(scene);
    }
    if (done.status == SCENE_OK) {
        done = open_lights(scene);
    }
    if (done.status == SCENE_OK) {
        done = open_animators(scene);
    }
    if (done.status == SCENE_OK) {
        done = open_skins(scene);
    }
    if (why != NULL) {
        *why = done;
    }
    if (done.status != SCENE_OK) {
        scene_animation_release(scene);
        memory_free(scene->light_transfer);
        scene_hierarchy_release(scene);
        scene_free_kept(scene);
        return NULL;
    }
    loaded[loaded_count++] = scene;
    return scene;
}

scene_t*
scene_load(const char* id, scene_failure_t* why) {
    const handle_t load = asset_store_load(id);
    const asset_pack_t* pack = asset_store_loaded(load);
    scene_t* scene = scene_load_from(pack, id, why);
    if (scene != NULL) {
        scene->load = load;
    } else {
        asset_store_release(load);
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
        asset_store_release(scene->load);
        loaded_count--;
        memmove(&loaded[i], &loaded[i + 1], sizeof loaded[0] * (size_t)(loaded_count - i));
        loaded[loaded_count] = NULL;
        scene_animation_release(scene);
        memory_free(scene->light_transfer);
        scene_hierarchy_release(scene);
        scene_free_kept(scene);
        if (loaded_count == 0) {
            /* Nothing left to draw: drop the scratch, so whoever lent it can
             * take it back. */
            render_context_drop_scratch(render_context_main());
        }
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
    return scene->asset.names[entity].name;
}

scene_entity_t
scene_find(const scene_t* scene, name_hash_t name) {
    for (int i = 0; i < scene->asset.entity_count; i++) {
        if (scene->asset.names[i].hash == name) {
            return (scene_entity_t)i;
        }
    }
    return SCENE_ENTITY_NONE;
}

/* The index of the entity's renderer, or -1 when it draws nothing. */
int
scene_renderer_index(const scene_t* scene, scene_entity_t entity) {
    assert(entity < scene->asset.entity_count);
    for (int i = 0; i < scene->renderer_count; i++) {
        if (scene->renderers[i].entity == entity) {
            return i;
        }
    }
    return -1;
}

const char*
scene_entity_mesh_id(const scene_t* scene, scene_entity_t entity) {
    const int i = scene_renderer_index(scene, entity);
    if (i < 0) {
        return NULL;
    }
    return i < scene->asset.renderer_count ? scene->asset.renderers[i].mesh
                                           : scene->asset.skinned_renderers[i - scene->asset.renderer_count].mesh;
}

const r3d_lit_mesh_t*
scene_entity_mesh(const scene_t* scene, scene_entity_t entity) {
    const int i = scene_renderer_index(scene, entity);
    return i < 0 ? NULL : &scene->renderers[i].mesh;
}

bool
scene_renderer_set_mesh(scene_t* scene, scene_entity_t entity, const r3d_lit_mesh_t* mesh) {
    const int i = scene_renderer_index(scene, entity);
    if (i < 0 || i >= scene->asset.renderer_count) {
        return false;
    }
    scene->renderers[i].mesh = *mesh;
    return true;
}

int
scene_camera_index(const scene_t* scene, name_hash_t camera) {
    for (int i = 0; i < scene->asset.camera_count; i++) {
        if (camera == SCENE_CAMERA_FIRST || scene->asset.names[scene->cameras[i].entity].hash == camera) {
            return i;
        }
    }
    return -1;
}

const camera_t*
scene_camera_lens(const scene_t* scene, name_hash_t camera) {
    const int index = scene_camera_index(scene, camera);
    return index < 0 ? NULL : &scene->cameras[index].lens;
}

static camera_t*
camera_by_entity(scene_t* scene, scene_entity_t entity) {
    if (scene != NULL) {
        for (int i = 0; i < scene->asset.camera_count; i++) {
            if (scene->cameras[i].entity == entity) {
                return &scene->cameras[i].lens;
            }
        }
    }
    return NULL;
}

bool
scene_camera_set_present(scene_t* scene, scene_entity_t camera, camera_present_t present) {
    camera_t* found = camera_by_entity(scene, camera);
    if (found == NULL || present < CAMERA_PRESENT_NEAREST || present > CAMERA_PRESENT_DOUBLE) {
        return false;
    }
    found->present = present;
    return true;
}

bool
scene_camera_set_shift(scene_t* scene, scene_entity_t camera, vec2f_t shift) {
    camera_t* found = camera_by_entity(scene, camera);
    if (found == NULL || !isfinite(shift.x) || !isfinite(shift.y)) {
        return false;
    }
    found->shift = shift;
    return true;
}

bool
scene_camera_set_small_fill(scene_t* scene, scene_entity_t camera, small_fill_t fill) {
    camera_t* found = camera_by_entity(scene, camera);
    if (found == NULL || fill.ceiling > SMALL_FILL_CEILING_MAX || fill.style < SMALL_FILL_FLAT
        || fill.style >= SMALL_FILL_STYLE_COUNT) {
        return false;
    }
    found->small_fill = fill;
    return true;
}

bool
scene_camera_set_skip_hidden(scene_t* scene, scene_entity_t camera, bool skip) {
    camera_t* found = camera_by_entity(scene, camera);
    if (found == NULL) {
        return false;
    }
    found->skip_hidden = skip;
    return true;
}

transformf_t
scene_entity_local(const scene_t* scene, scene_entity_t entity) {
    assert(entity < scene->asset.entity_count);
    return scene->locals[entity];
}

void
scene_entity_set_local(scene_t* scene, scene_entity_t entity, transformf_t local) {
    assert(entity < scene->asset.entity_count);
    /* A parent scales evenly, so every world below it is an exact TRS; the
     * loader and the importer keep content so. */
    assert(scene->world_slots[entity] == SCENE_WORLD_SLOT_NONE || transformf_scale_is_even(&local));
    scene->locals[entity] = local;
    scene->flags[entity] |= SCENE_FLAG_CHANGED;
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
    scene->flags[entity] |= SCENE_FLAG_CHANGED;
    scene_hierarchy_update_active(scene, entity);
}

bool
scene_transform_is_identity(const mat4f_t* transform) {
    const mat4f_t identity = mat4f_identity();
    return memcmp(transform, &identity, sizeof identity) == 0;
}

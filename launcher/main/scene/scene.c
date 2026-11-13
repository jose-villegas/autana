#include "scene/scene.h"

#include "scene/light/light.h"
#include "scene/renderer/renderer.h"

#include <assert.h>
#include <math.h>
#include <stdalign.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "asset/asset_id.h"
#include "asset/asset_store.h"
#include "core/memory.h"
#include "esp_log.h"
#include "gfx/target/gfx_color.h"
#include "image/pixel/rgb565.h"
#include "math/scalar/mathi.h"
#include "render/context/render_context.h"
#include "scene/scene_internal.h"

#if !defined(ESP_PLATFORM) || CONFIG_LAUNCHER_DEVELOPMENT
int scene_fail_allocation_after = -1;
#endif
static uint64_t requests; /* scene_root_t.order of the latest request */
static memory_take_fn kept_take;

size_t
scene_storage_carve(size_t* end, size_t bytes, size_t align) {
    const size_t at = mathi_size_ceil(*end, align);
    *end = at + bytes;
    return at;
}

/* What one of the scene's arrays is counted in. */
enum { PER_RENDERER, PER_CAMERA, PER_ENTITY, PER_BATCH, PER_COUNT };

#define PART(member, per)                                                                                              \
    {offsetof(scene_t, member), sizeof *((scene_t*)0)->member, _Alignof(__typeof__(*((scene_t*)0)->member)), per}

/* The scene's arrays, in block order after its header. */
static const struct {
    size_t member, size, align;
    int per;
} PARTS[] = {PART(renderers, PER_RENDERER),
             PART(cameras, PER_CAMERA),
             PART(flags, PER_ENTITY),
             PART(locals, PER_ENTITY),
             PART(world_slots, PER_ENTITY),
             PART(animators, PER_ENTITY),
             PART(skinned_renderers, PER_ENTITY),
             PART(lights, PER_ENTITY),
             PART(staged, PER_ENTITY),
             PART(active_renderers, PER_RENDERER),
             PART(batches, PER_BATCH)};

/* Carves the scene's arrays after its header, each on its own alignment, and
 * points them into `scene`'s block; a NULL scene only measures. Returns the
 * block's size. */
static size_t
place(const scene_asset_t* asset, scene_t* scene) {
    const size_t counts[PER_COUNT] = {[PER_RENDERER] = (size_t)asset->renderer_count + asset->skinned_renderer_count,
                                      [PER_CAMERA] = asset->camera_count,
                                      [PER_ENTITY] = asset->entity_count,
                                      [PER_BATCH] = asset->batch_count};
    size_t end = sizeof(scene_t);
    for (size_t i = 0; i < sizeof PARTS / sizeof *PARTS; i++) {
        const size_t at = scene_storage_carve(&end, PARTS[i].size * counts[PARTS[i].per], PARTS[i].align);
        if (scene != NULL) {
            void* array = (uint8_t*)scene + at;
            memcpy((uint8_t*)scene + PARTS[i].member, &array, sizeof array);
        }
    }
    return end;
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
    return kept_take != NULL ? kept_take(bytes, _Alignof(max_align_t)) : memory_alloc(bytes, MEMORY_APP);
}

void
scene_free_kept(void* block) {
    if (kept_take == NULL) {
        memory_free(block);
    }
}

memory_take_fn
scene_set_take(memory_take_fn take) {
    for (int i = 0; i < SCENE_ROOTS_MAX; i++) {
        assert(scene_roots[i].scene == NULL);
    }
    const memory_take_fn replaced = kept_take;
    kept_take = take;
    render_context_set_take(render_context_main(), take);
    return replaced;
}

static scene_t*
instantiate(const scene_asset_t* asset) {
    const size_t total = place(asset, NULL);
    uint8_t* block = scene_allocate_kept(total);
    if (block == NULL) {
        return NULL;
    }
    memset(block, 0, total);
    scene_t* scene = (scene_t*)(void*)block;
    scene->asset = *asset;
    scene->renderer_count = asset->renderer_count + asset->skinned_renderer_count;
    (void)place(asset, scene);
    scene->active_renderers_stale = true;
    scene->ambient_color = asset->settings->ambient_color;
    scene->ambient_intensity = asset->settings->ambient_intensity;
    scene->tonemap_white = asset->settings->tonemap_white;
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

/* Requests the pack `parts` names in `id`; a bare id names a pack of its own
 * name. Kept out of line, as is request_pack_async(), so the copy of the name
 * is off the stack while the scene opens. */
static __attribute__((noinline)) handle_t
request_pack(const char* id, asset_id_t parts) {
    char pack[ASSET_NAME_MAX];
    asset_id_pack(id, parts, id, pack);
    return asset_store_load(pack);
}

static __attribute__((noinline)) handle_t
request_pack_async(const char* id, asset_id_t parts, entity_t root) {
    char pack[ASSET_NAME_MAX];
    asset_id_pack(id, parts, id, pack);
    return asset_store_load_async(pack, NULL, root);
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

bool
scene_report(scene_failure_t* why, scene_status_t status, asset_status_t asset, const char* what) {
    if (why != NULL) {
        *why = scene_failure_make(status, asset, what);
    }
    return status == SCENE_OK;
}

/* The meshes a renderer draws: one, or its batch's copies. */
static size_t
drawn_copies(const scene_renderer_t* r) {
    return r->batch != NULL ? r->batch->copy_count : 1U;
}

/* One block of coarse bits for the meshes with levels, each renderer's
 * words its own and each copy's its own, all clear: every mesh starts at its
 * finest. */
static scene_failure_t
open_lod_bits(scene_t* scene) {
    size_t words = 0;
    for (int i = 0; i < scene->renderer_count; i++) {
        const scene_renderer_t* r = &scene->renderers[i];
        words += R3D_LOD_STATE_WORDS(r->mesh.lod_group_count) * (size_t)drawn_copies(r);
    }
    if (words == 0) {
        return scene_failure_make(SCENE_OK, ASSET_OK, scene->root->id);
    }
    scene->lod_bits = scene_allocate(words * sizeof(uint32_t), MEMORY_INTERNAL);
    if (scene->lod_bits == NULL) {
        return scene_failure_make(SCENE_ERR_MEMORY, ASSET_OK, scene->root->id);
    }
    memset(scene->lod_bits, 0, words * sizeof(uint32_t));
    uint32_t* next = scene->lod_bits;
    for (int i = 0; i < scene->renderer_count; i++) {
        scene_renderer_t* r = &scene->renderers[i];
        const size_t copy_words = R3D_LOD_STATE_WORDS(r->mesh.lod_group_count);
        r->lod_words = (uint32_t)copy_words * (uint32_t)drawn_copies(r);
        r->lod_coarse = r->lod_words > 0 ? next : NULL;
        if (r->batch != NULL) {
            r->batch->lod_coarse = r->lod_coarse;
            r->batch->lod_words = (uint16_t)copy_words;
        }
        next += r->lod_words;
    }
    return scene_failure_make(SCENE_OK, ASSET_OK, scene->root->id);
}

/* The copies of each batch: its renderer's mesh, opened at colour set 0,
 * checked against every copy's set; its chunks into internal RAM, the cull's
 * first read. */
static scene_failure_t
open_batches(scene_t* scene, const asset_pack_t* pack) {
    const scene_asset_t* asset = &scene->asset;
    if (asset->batch_count == 0) {
        return scene_failure_make(SCENE_OK, ASSET_OK, scene->root->id);
    }
    scene->chunks = scene_allocate(sizeof *scene->chunks * asset->chunk_count, MEMORY_INTERNAL);
    if (scene->chunks == NULL) {
        return scene_failure_make(SCENE_ERR_MEMORY, ASSET_OK, scene->root->id);
    }
    memcpy(scene->chunks, asset->chunks, sizeof *scene->chunks * asset->chunk_count);
    for (int i = 0; i < asset->batch_count; i++) {
        const scene_asset_batch_t* row = &asset->batches[i];
        scene_renderer_t* r = &scene->renderers[row->renderer];
        const char* mesh = asset->renderers[row->renderer].mesh;
        uint16_t sets = 0;
        for (int c = 0; c < row->copy_count; c++) {
            sets = asset->places[row->first_copy + c].colour_set >= sets
                       ? (uint16_t)(asset->places[row->first_copy + c].colour_set + 1U)
                       : sets;
        }
        /* The highest set opens only when every lower one is there too. */
        r3d_lit_mesh_t highest = r->mesh;
        highest.colors = NULL;
        highest.face_colors = NULL;
        const asset_status_t status = asset->renderers[row->renderer].colour_set != 0
                                          ? ASSET_ERR_FORMAT
                                          : r3d_lit_mesh_open_colours(pack, mesh, (uint16_t)(sets - 1U), &highest);
        if (status != ASSET_OK) {
            return scene_failure_make(SCENE_ERR_ASSET, status, mesh);
        }
        r3d_batch_t* batch = &scene->batches[i];
        *batch = (r3d_batch_t){.chunks = scene->chunks + row->first_chunk,
                               .places = asset->places + row->first_copy,
                               .turns = asset->turns + row->first_copy,
                               .colour_stride =
                                   (uint32_t)(r->mesh.colors != NULL ? r->mesh.vertex_count : r->mesh.triangle_count),
                               .copy_radius = r3d_batch_copy_radius(&r->mesh),
                               .copy_count = row->copy_count,
                               .chunk_count = (uint16_t)r3d_batch_chunks(row->copy_count)};
        r->batch = batch;
        r->colour_sets = sets;
    }
    return scene_failure_make(SCENE_OK, ASSET_OK, scene->root->id);
}

/* Opens every renderer's mesh, then their coarse bits; stops at the first that fails. */
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
        scene->renderers[i].colour_sets = 1;
    }
    const scene_failure_t batches = open_batches(scene, pack);
    return batches.status != SCENE_OK ? batches : open_lod_bits(scene);
}

static const char*
skeleton_for(const scene_t* scene, scene_row_t entity) {
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
        return scene_failure_make(SCENE_ERR_ASSET, ASSET_ERR_BOUNDS, scene->root->id);
    }
    const char* skeleton = skeleton_for(scene, row->entity);
    if (row->clip_count == 0 && skeleton == NULL) {
        return scene_failure_make(SCENE_OK, ASSET_OK, scene->root->id);
    }
    const char** clips = NULL;
    if (row->clip_count > 0) {
        clips = scene_allocate(sizeof *clips * row->clip_count, MEMORY_APP);
        if (clips == NULL) {
            return scene_failure_make(SCENE_ERR_MEMORY, ASSET_OK, scene->root->id);
        }
        for (int c = 0; c < row->clip_count; c++) {
            clips[c] = scene->asset.clips[row->first_clip + c];
        }
    }
    scene_failure_t why;
    const bool bound = animator_set_clips(scene_row_entity(scene, row->entity), skeleton, clips, row->clip_count, &why);
    memory_free(clips);
    if (!bound) {
        return why;
    }
    scene->animators[row->entity].wrap = (anim_wrap_t)row->wrap;
    if (row->default_clip != SCENE_ASSET_DEFAULT_CLIP_NONE) {
        (void)animator_play(scene_row_entity(scene, row->entity), row->default_clip, 0);
    }
    return scene_failure_make(SCENE_OK, ASSET_OK, scene->root->id);
}

static scene_failure_t
open_animators(scene_t* scene) {
    for (int i = 0; i < scene->asset.animator_count; i++) {
        const scene_failure_t why = open_animator(scene, &scene->asset.animators[i]);
        if (why.status != SCENE_OK) {
            return why;
        }
    }
    return scene_failure_make(SCENE_OK, ASSET_OK, scene->root->id);
}

static scene_failure_t
open_skins(scene_t* scene) {
    for (int i = 0; i < scene->asset.skinned_renderer_count; i++) {
        const scene_asset_skinned_renderer_t* row = &scene->asset.skinned_renderers[i];
        scene_failure_t why;
        if (scene->animators[row->entity].storage == NULL
            && !animator_set_clips(scene_row_entity(scene, row->entity), row->skeleton, NULL, 0, &why)) {
            return why;
        }
        /* Its buffers wait for the first step that finds it active. */
        if (!scene_renderer_bind_skin(scene, row->entity, row->skin, false, &why)) {
            return why;
        }
    }
    return scene_failure_make(SCENE_OK, ASSET_OK, scene->root->id);
}

static scene_failure_t
open_lights(scene_t* scene) {
    for (int i = 0; i < scene->asset.light_count; i++) {
        const scene_asset_light_t* row = &scene->asset.lights[i];
        if (!light_set_color(scene_row_entity(scene, row->entity), row->color)
            || !light_set_intensity(scene_row_entity(scene, row->entity), row->intensity)) {
            return scene_failure_make(SCENE_ERR_MEMORY, ASSET_OK, scene->asset.names[row->entity].name);
        }
    }
    return scene_failure_make(SCENE_OK, ASSET_OK, scene->root->id);
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
    return scene_failure_make(SCENE_OK, ASSET_OK, scene->root->id);
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

static void
free_columns(scene_t* scene) {
    scene_animation_release(scene);
    memory_free(scene->light_transfer);
    memory_free(scene->lod_bits);
    memory_free(scene->chunks);
    scene_hierarchy_release(scene);
    scene_free_kept(scene->resident);
    scene_free_kept(scene);
}

/* The record's columns and rows from the scene entry in `pack`. Its own
 * frame: the entry's view is gone before the meshes open. */
static __attribute__((noinline)) scene_failure_t
open_columns(scene_root_t* record, const asset_pack_t* pack, scene_t** opened) {
    scene_asset_t asset;
    const scene_failure_t read = open_asset(pack, record->id, &asset);
    if (read.status != SCENE_OK) {
        return read;
    }
    if (!scene_world_issue_rows(record, asset.entity_count)) {
        return scene_failure_make(SCENE_ERR_FULL, ASSET_OK, record->id);
    }
    *opened = instantiate(&asset);
    return scene_failure_make(*opened == NULL ? SCENE_ERR_MEMORY : SCENE_OK, ASSET_OK, record->id);
}

/* Opens the record's scene from `pack`: its columns, rows and components. */
static scene_failure_t
open_scene(scene_root_t* record, const asset_pack_t* pack) {
    scene_t* scene = NULL;
    scene_failure_t done = open_columns(record, pack, &scene);
    if (done.status != SCENE_OK) {
        return done;
    }
    scene->pack = pack;
    scene->root = record;
    record->scene = scene;
    scene_hierarchy_update_active(scene, SCENE_ROW_ROOT);
    done = scene_hierarchy_open(scene) ? open_meshes(scene, pack)
                                       : scene_failure_make(SCENE_ERR_MEMORY, ASSET_OK, record->id);
    if (done.status == SCENE_OK) {
        done = scene_mesh_make_resident(scene);
    }
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
    if (done.status != SCENE_OK) {
        record->scene = NULL;
        free_columns(scene);
    }
    return done;
}

static void
settle(scene_root_t* record, scene_failure_t done) {
    record->failure = done;
    record->state = done.status == SCENE_OK ? PENDING_READY : PENDING_FAILED;
    if (done.status != SCENE_OK) {
        record->rows = 0;
        asset_store_release(record->load);
        record->load = HANDLE_NONE;
        ESP_LOGW("scene", "%s did not load: status %d, asset %d, about %s", record->id, (int)done.status,
                 (int)done.asset, done.what);
    }
}

/* Whether any record holds a scene's columns. */
static bool
any_scene(void) {
    for (int i = 0; i < SCENE_ROOTS_MAX; i++) {
        if (scene_roots[i].scene != NULL) {
            return true;
        }
    }
    return false;
}

static void
forget(scene_root_t* record) {
    scene_t* scene = record->scene;
    if (scene != NULL) {
        scene_draw_forget(scene);
        free_columns(scene);
        record->scene = NULL;
        if (!any_scene()) {
            /* Nothing left to draw: drop the scratch, so whoever lent it can
             * take it back. */
            render_context_drop_scratch(render_context_main());
        }
    }
    asset_store_release(record->load);
    *record = (scene_root_t){0};
}

/* A free record, else the oldest cancelled or failed one, reclaimed; NULL
 * when every one holds a scene. */
static scene_root_t*
claim(const char* id) {
    scene_root_t* found = NULL;
    for (int i = 0; i < SCENE_ROOTS_MAX; i++) {
        scene_root_t* record = &scene_roots[i];
        if (handle_is_none(record->root)) {
            found = record;
            break;
        }
        const bool spent = record->state == PENDING_FAILED || record->state == PENDING_CANCELLED;
        if (spent && (found == NULL || record->order < found->order)) {
            found = record;
        }
    }
    if (found == NULL) {
        ESP_LOGW("scene", "%s not requested: %d scenes are held", id, SCENE_ROOTS_MAX);
        return NULL;
    }
    forget(found);
    if (!scene_world_issue_root(found)) {
        ESP_LOGW("scene", "%s not requested: no entity slot left", id);
        return NULL;
    }
    copy_id(found->id, id);
    found->state = PENDING_WAIT;
    found->order = ++requests;
    found->local = transformf_identity();
    found->world = mat4f_identity();
    found->flags = SCENE_FLAG_ENABLED | SCENE_FLAG_ACTIVE;
    return found;
}

entity_t
scene_load_from(const asset_pack_t* pack, const char* id) {
    scene_root_t* record = claim(id);
    if (record == NULL) {
        return ENTITY_NONE;
    }
    settle(record, open_scene(record, pack));
    return record->root;
}

entity_t
scene_load(const char* id) {
    asset_id_t parts;
    const bool named = asset_id_split(id, &parts);
    /* The record holds the scene's entry; its load holds the pack. */
    scene_root_t* record = claim(named ? parts.name : id);
    if (record == NULL) {
        return ENTITY_NONE;
    }
    if (!named) {
        settle(record, scene_failure_make(SCENE_ERR_ASSET, ASSET_ERR_FORMAT, id));
        return record->root;
    }
    record->load = request_pack(id, parts);
    settle(record, open_scene(record, asset_store_loaded(record->load)));
    return record->root;
}

/* The record whose root is `root`, or NULL. */
static scene_root_t*
record_of(entity_t root) {
    scene_row_t row;
    scene_root_t* record = scene_entity_peek(root, &row);
    return record != NULL && row == SCENE_ROW_ROOT ? record : NULL;
}

entity_t
scene_load_async(const char* id, pending_fn done, handle_t owner) {
    asset_id_t parts;
    const bool named = asset_id_split(id, &parts);
    scene_root_t* record = claim(named ? parts.name : id);
    if (record == NULL) {
        return ENTITY_NONE;
    }
    record->async = true;
    record->done = done;
    record->owner = owner;
    if (!named) {
        settle(record, scene_failure_make(SCENE_ERR_ASSET, ASSET_ERR_FORMAT, id));
        return record->root;
    }
    /* The root owns its pack request: releasing the root cancels it. */
    record->load = request_pack_async(id, parts, record->root);
    if (!pending_owner_alive(owner)) {
        scene_cancel(record->root);
    }
    return record->root;
}

pending_t
scene_state(entity_t root) {
    scene_row_t row;
    const scene_root_t* record = scene_entity_peek(root, &row);
    return record == NULL || row != SCENE_ROW_ROOT ? PENDING_NONE : record->state;
}

bool
scene_failure(entity_t root, scene_failure_t* why) {
    const scene_root_t* record = record_of(root);
    const bool starved =
        record != NULL && record->state == PENDING_READY && record->scene != NULL && record->scene->starved_count > 0;
    if (record == NULL || (record->state != PENDING_FAILED && !starved)) {
        return false;
    }
    *why = record->failure;
    return true;
}

void
scene_cancel(entity_t root) {
    scene_root_t* record = record_of(root);
    if (record == NULL) {
        return;
    }
    record->done = NULL;
    if (record->state == PENDING_WAIT) {
        asset_store_release(record->load);
        record->load = HANDLE_NONE;
        record->state = PENDING_CANCELLED;
    }
}

void
scene_release(entity_t root) {
    scene_root_t* record = record_of(root);
    if (record != NULL) {
        forget(record);
    }
}

static void
step_request(scene_root_t* record) {
    if (!pending_owner_alive(record->owner)) {
        scene_cancel(record->root);
        return;
    }
    if (record->state == PENDING_WAIT) {
        const pending_t pack = asset_store_state(record->load);
        if (pack == PENDING_READY) {
            settle(record, open_scene(record, asset_store_loaded(record->load)));
        } else if (pack != PENDING_WAIT) {
            settle(record, scene_failure_make(SCENE_ERR_ASSET, ASSET_ERR_NO_PACK, record->id));
        }
    }
    if ((record->state == PENDING_READY || record->state == PENDING_FAILED) && record->done != NULL) {
        const pending_fn done = record->done;
        record->done = NULL;
        done(record->root, record->state, record->owner);
    }
}

void
scene_step(uint32_t dt_ms) {
    (void)dt_ms;
    /* Requests made inside a callback wait for the next pass. */
    const uint64_t cutoff = requests;
    for (int i = 0; i < SCENE_ROOTS_MAX; i++) {
        scene_root_t* record = &scene_roots[i];
        const bool pending = record->state == PENDING_WAIT || record->done != NULL;
        if (record->async && pending && record->order <= cutoff && !handle_is_none(record->root)) {
            step_request(record);
        }
    }
}

void
scene_unload_all(void) {
    for (int i = 0; i < SCENE_ROOTS_MAX; i++) {
        forget(&scene_roots[i]);
    }
    scene_draw_release();
}

/* The index of the entity's renderer, or -1 when it draws nothing. */
int
scene_renderer_index(const scene_t* scene, scene_row_t entity) {
    assert(entity < scene->asset.entity_count);
    for (int i = 0; i < scene->renderer_count; i++) {
        if (scene->renderers[i].entity == entity) {
            return i;
        }
    }
    return -1;
}

bool
scene_transform_is_identity(const mat4f_t* transform) {
    const mat4f_t identity = mat4f_identity();
    return memcmp(transform, &identity, sizeof identity) == 0;
}

#if BUILD_VARIANT_DEVELOPMENT_OR_HOST
void
scene_checkpoint_capture(checkpoint_t* saved, scene_t* scene) {
    checkpoint_capture(saved, scene, place(&scene->asset, NULL));
    checkpoint_capture_at(saved, &scene->worlds, (size_t)scene->world_count * sizeof(*scene->worlds));
    checkpoint_capture_at(saved, &scene->light_transfer, sizeof(*scene->light_transfer));
    checkpoint_capture_at(saved, &scene->light_table, sizeof(*scene->light_table));
    scene_animation_checkpoint_capture(saved, scene);
}
#endif

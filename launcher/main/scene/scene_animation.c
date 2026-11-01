/* Scene animation owns component bindings and buffers; entity and cluster jobs synchronize before bounds refit. */
#include <assert.h>
#include <stdalign.h>
#include <stdlib.h>
#include <string.h>
#include "anim/anim_tracks_format.h"
#include "core/job.h"
#include "core/memory.h"
#include "math/scalar/mathi.h"
#include "scene/scene_internal.h"

typedef struct {
    anim_target_t* targets;
    anim_component_ref_t* components;
    void* block;
    int count;
} targets_t;

static targets_t
targets_for(scene_t* scene, scene_entity_t owner) {
    const int entities = scene->asset.entity_count;
    size_t end = 0;
    const size_t targets = scene_storage_carve(&end, sizeof(anim_target_t) * (size_t)entities, alignof(anim_target_t));
    const size_t components =
        scene_storage_carve(&end, sizeof(anim_component_ref_t) * (size_t)entities * 2, alignof(anim_component_ref_t));
    uint8_t* block = memory_alloc(end, MEMORY_PSRAM);
    targets_t out = {.block = block, .count = entities};
    if (block == NULL) {
        return out;
    }
    out.targets = (anim_target_t*)(void*)(block + targets);
    out.components = (anim_component_ref_t*)(void*)(block + components);
    for (int e = 0; e < entities; e++) {
        anim_component_ref_t* fields = out.components + (e * 2);
        fields[0] = (anim_component_ref_t){&ANIM_POSE_TRANSFORM_FIELDS, &scene->locals[e]};
        out.targets[e] = (anim_target_t){scene->asset.names[e].hash, fields, 1, &scene->flags[e], SCENE_FLAG_CHANGED};
    }
    for (int c = 0; c < scene->asset.camera_count; c++) {
        const int e = scene->cameras[c].entity;
        out.components[(e * 2) + 1] = (anim_component_ref_t){&CAMERA_FIELDS, &scene->cameras[c].lens};
        out.targets[e].component_count = 2;
    }
    const anim_target_t first = out.targets[0];
    out.targets[0] = out.targets[owner];
    out.targets[owner] = first;
    return out;
}

static bool
finish(scene_failure_t* why, scene_status_t status, asset_status_t asset, const char* what) {
    if (why != NULL) {
        *why = scene_failure_make(status, asset, what);
    }
    return status == SCENE_OK;
}

typedef struct {
    size_t clips, times, counts, tables, names, joints, fields, shown, frozen, total;
} animator_layout_t;

static animator_layout_t
layout_for(int clips, int curves, int joints) {
    animator_layout_t l;
    size_t end = 0;
    SCENE_STORAGE_RESERVE(l, end, clips, scene_animator_clip_t, clips);
    SCENE_STORAGE_RESERVE(l, end, times, anim_clip_t, clips);
    SCENE_STORAGE_RESERVE(l, end, counts, int, clips);
    SCENE_STORAGE_RESERVE(l, end, tables, const anim_joint_bound_t*, clips);
    SCENE_STORAGE_RESERVE(l, end, names, char, clips* ASSET_NAME_MAX);
    SCENE_STORAGE_RESERVE(l, end, joints, anim_joint_bound_t, joints == 0 ? 0 : curves);
    SCENE_STORAGE_RESERVE(l, end, fields, anim_bound_t, joints == 0 ? curves : 0);
    SCENE_STORAGE_RESERVE(l, end, shown, transformf_t, joints);
    SCENE_STORAGE_RESERVE(l, end, frozen, transformf_t, joints);
    l.total = end;
    return l;
}

static bool
allocate_animator(scene_animator_t* a, int clips, int curves) {
    const animator_layout_t l = layout_for(clips, curves, a->skeleton.joint_count);
    uint8_t* block = scene_allocate_kept(l.total);
    if (block == NULL) {
        return false;
    }
    memset(block, 0, l.total);
    a->storage = block;
    a->clips = (scene_animator_clip_t*)(void*)(block + l.clips);
    a->counts = (int*)(void*)(block + l.counts);
    a->joint_tables = (const anim_joint_bound_t**)(void*)(block + l.tables);
    const uint8_t joints = a->skeleton.joint_count;
    anim_layer_init(&a->layer, (anim_clip_t*)(void*)(block + l.times), clips,
                    (anim_pose_t){(transformf_t*)(void*)(block + l.shown), joints},
                    (anim_pose_t){(transformf_t*)(void*)(block + l.frozen), joints});
    return true;
}

static asset_status_t
bind_clips(scene_t* scene, scene_animator_t* a, const char* const* ids, int curves, scene_entity_t owner,
           const char** failed) {
    const animator_layout_t l = layout_for(a->layer.clip_count, curves, a->skeleton.joint_count);
    uint8_t* block = a->storage;
    targets_t targets = {0};
    if (a->root == ANIM_ROOT_SCENE) {
        targets = targets_for(scene, owner);
        if (targets.block == NULL) {
            return ASSET_ERR_FULL;
        }
    }
    int first = 0;
    asset_status_t result = ASSET_OK;
    for (int c = 0; c < a->layer.clip_count; c++) {
        scene_animator_clip_t* clip = &a->clips[c];
        (void)anim_tracks_from_pack(scene->pack, ids[c], &clip->tracks);
        char* name = (char*)block + l.names + (c * ASSET_NAME_MAX);
        /* The clip was opened from the pack, whose names fit ASSET_NAME_MAX. */
        const size_t length = strlen(ids[c]);
        assert(length < ASSET_NAME_MAX);
        memcpy(name, ids[c], length + 1);
        clip->id = name;
        ((anim_clip_t*)(void*)a->layer.clips)[c] = clip->tracks.clip;
        a->counts[c] = clip->tracks.count;
        clip->joints = (anim_joint_bound_t*)(void*)(block + l.joints) + first;
        a->joint_tables[c] = clip->joints;
        anim_bind_status_t status;
        if (a->root == ANIM_ROOT_SKELETON) {
            status = anim_bind_skeleton(&clip->tracks, &a->skeleton, clip->joints, a->counts[c], NULL);
        } else {
            clip->fields = (anim_bound_t*)(void*)(block + l.fields) + first;
            status = anim_bind(&clip->tracks, targets.targets, targets.count, clip->fields, a->counts[c], NULL);
        }
        if (status != ANIM_BIND_OK) {
            *failed = ids[c];
            result = ASSET_ERR_FORMAT;
            break;
        }
        first += a->counts[c];
    }
    memory_free(targets.block);
    return result;
}

static asset_status_t
count_curves(const scene_t* scene, uint8_t root, const char* const* ids, int count, int* curves, const char** failed) {
    for (int c = 0; c < count; c++) {
        anim_tracks_t clip;
        *failed = ids[c];
        const asset_status_t status = anim_tracks_from_pack(scene->pack, ids[c], &clip);
        if (status != ASSET_OK) {
            return status;
        }
        if (clip.root != root) {
            return ASSET_ERR_FORMAT;
        }
        *curves += clip.count;
    }
    return ASSET_OK;
}

static bool
clip_count_fits(int count, const char* skeleton_id) {
    return count >= 0 && count <= ANIM_LAYER_CLIP_NONE && (count > 0 || skeleton_id != NULL);
}

bool
scene_animator_set_clips(scene_t* scene, scene_entity_t entity, const char* skeleton_id, const char* const* ids,
                         int count, scene_failure_t* why) {
    if (entity >= scene->asset.entity_count || !clip_count_fits(count, skeleton_id)) {
        return finish(why, SCENE_ERR_ASSET, ASSET_ERR_BOUNDS, scene->id);
    }
    scene_animator_t a = {.root = skeleton_id == NULL ? ANIM_ROOT_SCENE : ANIM_ROOT_SKELETON, .wrap = ANIM_LOOP};
    asset_status_t status = skeleton_id == NULL ? ASSET_OK : anim_skeleton_open(scene->pack, skeleton_id, &a.skeleton);
    const char* failed = skeleton_id == NULL ? ids[0] : skeleton_id;
    int curves = 0;
    if (status == ASSET_OK) {
        status = count_curves(scene, a.root, ids, count, &curves, &failed);
    }
    scene_skinned_renderer_t* skin = &scene->skinned_renderers[entity];
    if (skin->internal_storage != NULL && skin->skin.joint_count != a.skeleton.joint_count) {
        status = ASSET_ERR_FORMAT;
    }
    if (status != ASSET_OK) {
        return finish(why, SCENE_ERR_ASSET, status, failed);
    }
    if (!allocate_animator(&a, count, curves)) {
        return finish(why, SCENE_ERR_MEMORY, ASSET_OK, failed);
    }
    status = bind_clips(scene, &a, ids, curves, entity, &failed);
    if (status != ASSET_OK) {
        scene_free_kept(a.storage);
        return finish(why, SCENE_ERR_ASSET, status, failed);
    }
    if (a.root == ANIM_ROOT_SKELETON) {
        a.posed = true;
        anim_pose_rest(&a.skeleton, &a.layer.shown);
        anim_pose_rest(&a.skeleton, &a.layer.frozen);
        if (skin->internal_storage != NULL) {
            anim_pose_model(&a.skeleton, &a.layer.shown, skin->model);
            r3d_skin_palette(&skin->skin, skin->model, skin->bind.position_scale, skin->palette);
        }
    }
    const scene_animator_t* old = &scene->animators[entity];
    scene->skeleton_animator_count +=
        (a.root == ANIM_ROOT_SKELETON) - (old->storage != NULL && old->root == ANIM_ROOT_SKELETON);
    scene->animator_count += old->storage == NULL;
    /* A taken block cannot be freed: rebinding a scene's clips abandons the
     * old storage to the lender until the scene's memory goes back. */
    scene_free_kept(old->storage);
    a.dirty = true;
    scene->animators[entity] = a;
    return finish(why, SCENE_OK, ASSET_OK, scene->id);
}

static asset_status_t
open_renderer_skin(scene_t* scene, scene_entity_t entity, int renderer, const char* id,
                   scene_skinned_renderer_t* next) {
    asset_status_t status = r3d_skin_open(scene->pack, id, &next->skin);
    const scene_animator_t* animator = &scene->animators[entity];
    const scene_skinned_renderer_t* old = &scene->skinned_renderers[entity];
    next->bind = old->internal_storage == NULL ? scene->renderers[renderer].mesh : old->bind;
    if (status == ASSET_OK
        && (next->skin.joint_count != animator->skeleton.joint_count
            || next->skin.vertex_count != (uint32_t)next->bind.vertex_count)) {
        status = ASSET_ERR_FORMAT;
    }
    return status;
}

static bool
prepare_skin_lighting(scene_t* scene, scene_entity_t entity, scene_skinned_renderer_t* next, bool lit) {
    r3d_skin_light_transfer_t* transfer = NULL;
    if (lit && scene->light_transfer == NULL) {
        transfer = scene_light_transfer_take(scene);
        if (transfer == NULL) {
            return false;
        }
    }
    if (!scene_skin_light_apply_mode(scene, entity, next, (scene_skin_light_mode_t)next->skin.lighting_mode)) {
        memory_free(transfer);
        return false;
    }
    if (transfer != NULL) {
        scene->light_transfer = transfer;
    }
    return true;
}

static void
attach_renderer_skin(scene_t* scene, scene_entity_t entity, int renderer, const scene_skinned_renderer_t* next) {
    scene_skin_light_table_release(scene, entity);
    memory_free(scene->skinned_renderers[entity].light_storage);
    memory_free(scene->skinned_renderers[entity].internal_storage);
    scene->skinned_renderers[entity] = *next;
    scene->renderers[renderer].mesh = next->output.mesh;
    if (scene->light_transfer != NULL) {
        scene->skinned_renderers[entity].output.mesh.colors = next->colors;
        scene->renderers[renderer].mesh.colors = next->colors;
    }
    scene->flags[entity] |= SCENE_FLAG_SKIN_DIRTY;
}

bool
scene_renderer_set_skin(scene_t* scene, scene_entity_t entity, const char* id, scene_failure_t* why) {
    const int renderer = scene_renderer_index(scene, entity);
    if (renderer < 0) {
        return finish(why, SCENE_ERR_ASSET, ASSET_ERR_NOT_FOUND, id);
    }
    /* Its normals are lit in its own frame, so a skinned renderer is a root. */
    if (scene_entity_parent(scene, entity) != SCENE_ENTITY_NONE) {
        return finish(why, SCENE_ERR_ASSET, ASSET_ERR_FORMAT, id);
    }
    scene_skinned_renderer_t next = {0};
    const asset_status_t status = open_renderer_skin(scene, entity, renderer, id, &next);
    if (status != ASSET_OK) {
        return finish(why, SCENE_ERR_ASSET, status, id);
    }
    const bool lit = scene->light_count > 0 || scene->ambient_intensity > 0.0F;
    if (lit && next.bind.colors == NULL) {
        return finish(why, SCENE_ERR_ASSET, ASSET_ERR_FORMAT, SCENE_LIGHT_FAILURE_ALBEDO);
    }
    const size_t size = r3d_skin_output_bytes(&next.bind);
    size_t end = size;

    struct {
        size_t model, palette;
    } layout;

    SCENE_STORAGE_RESERVE(layout, end, model, mat4f_t, next.skin.joint_count);
    SCENE_STORAGE_RESERVE(layout, end, palette, mat4f_t, next.skin.joint_count);
    next.internal_storage = memory_alloc(end, MEMORY_INTERNAL);
    if (next.internal_storage == NULL) {
        return finish(why, SCENE_ERR_MEMORY, ASSET_OK, id);
    }
    if (lit && !scene_skin_light_allocate(&next)) {
        memory_free(next.internal_storage);
        return finish(why, SCENE_ERR_MEMORY, ASSET_OK, id);
    }
    (void)r3d_skin_output_init(&next.bind, next.internal_storage, size, &next.output);
    next.model = (mat4f_t*)(void*)((uint8_t*)next.internal_storage + layout.model);
    next.palette = (mat4f_t*)(void*)((uint8_t*)next.internal_storage + layout.palette);
    const scene_animator_t* animator = &scene->animators[entity];
    anim_pose_model(&animator->skeleton, &animator->layer.shown, next.model);
    r3d_skin_palette(&next.skin, next.model, next.bind.position_scale, next.palette);
    r3d_skin_clusters(&next.skin, &next.bind, next.palette, 0, next.bind.cluster_count, &next.output);
    r3d_skin_refit_nodes(&next.output);
    if (!prepare_skin_lighting(scene, entity, &next, lit)) {
        memory_free(next.light_storage);
        memory_free(next.internal_storage);
        return finish(why, SCENE_ERR_MEMORY, ASSET_OK, id);
    }
    attach_renderer_skin(scene, entity, renderer, &next);
    return finish(why, SCENE_OK, ASSET_OK, id);
}

static scene_animator_t*
animator_of(scene_t* scene, scene_entity_t entity) {
    if (entity >= scene->asset.entity_count || scene->animators[entity].storage == NULL) {
        return NULL;
    }
    return &scene->animators[entity];
}

bool
scene_animator_play(scene_t* scene, scene_entity_t entity, int clip, uint32_t blend_ms) {
    scene_animator_t* a = animator_of(scene, entity);
    if (a == NULL || clip < 0 || clip >= a->layer.clip_count || (a->root == ANIM_ROOT_SCENE && blend_ms != 0)) {
        return false;
    }
    anim_layer_play(&a->layer, (uint8_t)clip, blend_ms);
    a->layer.current.playhead.wrap = a->wrap;
    a->dirty = true;
    return true;
}

static anim_playhead_t*
slot_playhead(scene_animator_t* a, scene_animator_slot_t slot) {
    if (slot == SCENE_ANIMATOR_CURRENT) {
        return &a->layer.current.playhead;
    }
    return slot == SCENE_ANIMATOR_PREVIOUS && !a->layer.frozen_previous
                   && a->layer.previous.clip != ANIM_LAYER_CLIP_NONE
               ? &a->layer.previous.playhead
               : NULL;
}

bool
scene_animator_set_speed(scene_t* scene, scene_entity_t entity, scene_animator_slot_t slot, float speed) {
    scene_animator_t* a = animator_of(scene, entity);
    anim_playhead_t* p = a == NULL ? NULL : slot_playhead(a, slot);
    return p != NULL && anim_playhead_set_speed(p, speed);
}

bool
scene_animator_set_paused(scene_t* scene, scene_entity_t entity, scene_animator_slot_t slot, bool paused) {
    scene_animator_t* a = animator_of(scene, entity);
    anim_playhead_t* p = a == NULL ? NULL : slot_playhead(a, slot);
    if (p == NULL) {
        return false;
    }
    p->paused = paused;
    return true;
}

int
scene_animator_clip_count(const scene_t* scene, scene_entity_t entity) {
    return entity >= scene->asset.entity_count || scene->animators[entity].storage == NULL
               ? 0
               : scene->animators[entity].layer.clip_count;
}

int
scene_animator_current_clip(const scene_t* scene, scene_entity_t entity) {
    return scene_animator_clip_count(scene, entity) == 0
                   || scene->animators[entity].layer.current.clip == ANIM_LAYER_CLIP_NONE
               ? -1
               : scene->animators[entity].layer.current.clip;
}

const char*
scene_animator_clip_name(const scene_t* scene, scene_entity_t entity, int clip) {
    return clip < 0 || clip >= scene_animator_clip_count(scene, entity) ? NULL
                                                                        : scene->animators[entity].clips[clip].id;
}

void
scene_animation_release(scene_t* scene) {
    for (int e = 0; e < scene->asset.entity_count; e++) {
        scene_free_kept(scene->animators[e].storage);
        scene_skin_light_table_release(scene, (scene_entity_t)e);
        memory_free(scene->skinned_renderers[e].light_storage);
        memory_free(scene->skinned_renderers[e].internal_storage);
        scene->animators[e] = (scene_animator_t){0};
        scene->skinned_renderers[e] = (scene_skinned_renderer_t){0};
    }
    scene->animator_count = 0;
    scene->skeleton_animator_count = 0;
}

static bool
advance_animator(scene_animator_t* a, uint32_t dt_ms) {
    const uint32_t current = a->layer.current.playhead.t_ms;
    const uint32_t previous = a->layer.previous.playhead.t_ms;
    const uint32_t blended = a->layer.blended_ms;
    anim_layer_advance(&a->layer, dt_ms);
    const bool changed = a->dirty || current != a->layer.current.playhead.t_ms
                         || previous != a->layer.previous.playhead.t_ms || blended != a->layer.blended_ms;
    a->dirty = false;
    return changed;
}

void
scene_animate_range(scene_t* scene, int first, int count, uint32_t dt_ms) {
    for (int e = first; e < first + count; e++) {
        scene_animator_t* a = &scene->animators[e];
        a->posed = false;
        if (a->storage == NULL || a->root != ANIM_ROOT_SKELETON || a->layer.current.clip == ANIM_LAYER_CLIP_NONE
            || !(scene->flags[e] & SCENE_FLAG_ACTIVE)) {
            continue;
        }
        a->posed = advance_animator(a, dt_ms);
        if (!a->posed) {
            continue;
        }
        const anim_pose_t* pose = anim_layer_pose(&a->layer, &a->skeleton, a->joint_tables, a->counts);
        scene_skinned_renderer_t* skin = &scene->skinned_renderers[e];
        if (skin->internal_storage != NULL) {
            anim_pose_model(&a->skeleton, pose, skin->model);
            r3d_skin_palette(&skin->skin, skin->model, skin->bind.position_scale, skin->palette);
        }
    }
}

static void
apply_property(scene_t* scene, scene_animator_t* a, uint32_t dt_ms) {
    if (!advance_animator(a, dt_ms)) {
        return;
    }
    const anim_slot_t* slot = &a->layer.current;
    const scene_animator_clip_t* clip = &a->clips[slot->clip];
    anim_apply(clip->fields, clip->tracks.count, anim_playhead_seconds(&slot->playhead));
    for (int i = 0; i < clip->tracks.count; i++) {
        const ptrdiff_t entity = clip->fields[i].dirty - scene->flags;
        scene->locals[entity].cached = false;
    }
}

static const r3d_skin_light_kernel_t LIGHT_KERNELS[] = {
    [SCENE_SKIN_LIGHT_DIRECT] = r3d_skin_clusters_lit,
    [SCENE_SKIN_LIGHT_RENORMALISED] = r3d_skin_clusters_renormalised,
    [SCENE_SKIN_LIGHT_TABLE] = r3d_skin_clusters_table,
};
_Static_assert(sizeof LIGHT_KERNELS / sizeof LIGHT_KERNELS[0] == SCENE_SKIN_LIGHT_TABLE + 1, "lighting mode count");

void
scene_skin_range(scene_t* scene, int first, int count) {
    int offset = 0;
    for (int e = 0; e < scene->asset.entity_count; e++) {
        scene_skinned_renderer_t* skin = &scene->skinned_renderers[e];
        if (skin->internal_storage == NULL || !scene->animators[e].posed) {
            continue;
        }
        const int clusters = skin->bind.cluster_count;
        const int start = mathi_clamp(first - offset, 0, clusters);
        const int end = mathi_clamp(first + count - offset, 0, clusters);
        if (scene->light_transfer != NULL && skin->lighting_valid) {
            LIGHT_KERNELS[skin->light_mode](&skin->skin, &skin->bind, skin->palette, start, end - start, &skin->output,
                                            skin->normal_palette, &scene->world_lights, skin->colors);
        } else {
            r3d_skin_clusters(&skin->skin, &skin->bind, skin->palette, start, end - start, &skin->output);
        }
        offset += clusters;
    }
}

typedef struct {
    scene_t* scene;
    uint32_t dt_ms;
} animate_work_t;

static void
animate_slice(void* ctx, int from, int to) {
    const animate_work_t* w = ctx;
    scene_animate_range(w->scene, from, to - from, w->dt_ms);
}

static void
skin_slice(void* ctx, int from, int to) {
    scene_skin_range(ctx, from, to - from);
}

static void
animate_entities(scene_t* scene, uint32_t dt_ms) {
    if (scene->skeleton_animator_count == 0) {
        return;
    }
    animate_work_t w = {scene, dt_ms};
    (void)job_run_range(&scene->animate_share, animate_slice, &w, scene->asset.entity_count);
}

static bool
skin_lights_changed(const scene_t* scene) {
    const int entities = scene->asset.entity_count;
    if (scene->light_count == 0) {
        return false;
    }
    bool lights_dirty = false;
    for (int e = 0; e < entities; e++) {
        lights_dirty |= scene->lights[e].kind != SCENE_LIGHT_NONE && (scene->flags[e] & SCENE_FLAG_SKIN_DIRTY);
    }
    return lights_dirty;
}

static int
skin_stage_prepare(scene_t* scene, uint32_t dt_ms) {
    const int entities = scene->asset.entity_count;
    int clusters = 0;
    for (int e = 0; e < entities; e++) {
        scene_animator_t* animator = &scene->animators[e];
        if (animator->storage != NULL && animator->root == ANIM_ROOT_SCENE
            && animator->layer.current.clip != ANIM_LAYER_CLIP_NONE && (scene->flags[e] & SCENE_FLAG_ACTIVE)) {
            apply_property(scene, animator, dt_ms);
        }
    }
    const bool lights_dirty = skin_lights_changed(scene);
    scene->light_table_dirty |= lights_dirty;
    for (int e = 0; e < entities; e++) {
        scene_animator_t* animator = &scene->animators[e];
        scene_skinned_renderer_t* skin = &scene->skinned_renderers[e];
        if (!(scene->flags[e] & SCENE_FLAG_ACTIVE)) {
            continue;
        }
        if (skin->internal_storage != NULL && scene->light_transfer != NULL) {
            animator->posed |= lights_dirty || (scene->flags[e] & SCENE_FLAG_SKIN_DIRTY);
        }
        if (skin->internal_storage != NULL && animator->posed) {
            scene_skin_light_prepare(scene, (scene_entity_t)e);
            clusters += skin->bind.cluster_count;
        }
    }
    for (int e = 0; e < entities; e++) {
        scene->flags[e] &= (uint8_t)~SCENE_FLAG_SKIN_DIRTY;
    }
    return clusters;
}

void
scene_animation_step(scene_t* scene, uint32_t dt_ms) {
    if (scene->animator_count == 0) {
        return;
    }
    animate_entities(scene, dt_ms);
    const int clusters = skin_stage_prepare(scene, dt_ms);
    const int entities = scene->asset.entity_count;
    if (clusters == 0) {
        return;
    }
    if (scene->light_transfer != NULL) {
        scene_lights_prepare(scene);
    }
    (void)job_run_range(&scene->skin_share, skin_slice, scene, clusters);
    for (int e = 0; e < entities; e++) {
        if (scene->skinned_renderers[e].internal_storage != NULL && scene->animators[e].posed) {
            r3d_skin_refit_nodes(&scene->skinned_renderers[e].output);
        }
    }
}

uint32_t
scene_animator_clip_duration_ms(const scene_t* scene, scene_entity_t entity, int clip) {
    return clip < 0 || clip >= scene_animator_clip_count(scene, entity)
               ? 0
               : scene->animators[entity].clips[clip].tracks.clip.duration_ms;
}

bool
scene_animator_seek(scene_t* scene, scene_entity_t entity, scene_animator_slot_t slot, uint32_t t_ms) {
    scene_animator_t* a = animator_of(scene, entity);
    anim_playhead_t* p = a == NULL ? NULL : slot_playhead(a, slot);
    if (p == NULL || a->layer.current.clip == ANIM_LAYER_CLIP_NONE) {
        return false;
    }
    scene_draw_camera_cut(scene, entity);
    p->t_ms = t_ms;
    p->fraction = 0;
    a->dirty = true;
    scene_animation_step(scene, 0);
    return true;
}

#if BUILD_VARIANT_DEVELOPMENT_OR_HOST
void
scene_animation_checkpoint_capture(checkpoint_t* saved, scene_t* scene) {
    const size_t entities = scene->asset.entity_count;
    for (size_t e = 0; e < entities; e++) {
        scene_animator_t* a = &scene->animators[e];
        checkpoint_capture_at(saved, &a->layer.shown.local,
                              (size_t)a->skeleton.joint_count * sizeof(*a->layer.shown.local));
        checkpoint_capture_at(saved, &a->layer.frozen.local,
                              (size_t)a->skeleton.joint_count * sizeof(*a->layer.frozen.local));
        scene_skinned_renderer_t* skin = &scene->skinned_renderers[e];
        const size_t joints = skin->skin.joint_count;
        const size_t internal_bytes =
            skin->internal_storage == NULL
                ? 0
                : (size_t)((uint8_t*)(skin->palette + joints) - (uint8_t*)skin->internal_storage);
        const size_t light_bytes =
            skin->light_storage == NULL
                ? 0
                : (size_t)((uint8_t*)(skin->normal_palette + joints) - (uint8_t*)skin->light_storage);
        checkpoint_capture_at(saved, &skin->internal_storage, internal_bytes);
        checkpoint_capture_at(saved, &skin->light_storage, light_bytes);
    }
}
#endif

/* Scene animation owns component bindings and buffers; entity and cluster jobs synchronize before bounds refit. */
#include <assert.h>
#include <stdalign.h>
#include <stdlib.h>
#include <string.h>
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
    const size_t targets = scene_storage_carve(
        &end, sizeof(anim_target_t) * (size_t)(entities + scene->asset.camera_count), alignof(anim_target_t));
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
        out.targets[e] = (anim_target_t){scene->asset.names[e], fields, 1, &scene->flags[e], SCENE_FLAG_DIRTY};
    }
    anim_target_t own_alias = {0};
    for (int c = 0; c < scene->asset.camera_count; c++) {
        const int e = scene->cameras[c].entity;
        out.components[(e * 2) + 1] = (anim_component_ref_t){&R3D_SCENE_CAMERA_FIELDS, &scene->cameras[c].lens};
        out.targets[e].component_count = 2;
        const char* alias = scene->asset.cameras[c].node;
        if (e == owner && alias[0] != '\0' && strcmp(alias, scene->asset.names[e]) != 0) {
            own_alias = out.targets[e];
            own_alias.name = alias;
        }
    }
    if (own_alias.name != NULL) {
        memmove(out.targets + 1, out.targets, (size_t)out.count * sizeof *out.targets);
        out.targets[0] = own_alias;
        out.count++;
    }
    return out;
}

static bool
finish(scene_failure_t* why, scene_failure_t result) {
    if (why != NULL) {
        *why = result;
    }
    return result.status == SCENE_OK;
}

typedef struct {
    size_t clips, times, counts, tables, names, joints, fields, sampled, values, shown, frozen, model, palette, total;
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
    SCENE_STORAGE_RESERVE(l, end, joints, anim_joint_bound_t, curves);
    SCENE_STORAGE_RESERVE(l, end, fields, anim_bound_t, joints == 0 ? curves : 0);
    SCENE_STORAGE_RESERVE(l, end, sampled, anim_bound_t, joints == 0 ? curves : 0);
    SCENE_STORAGE_RESERVE(l, end, values, float, joints == 0 ? curves * ANIM_WIDTH_MAX : 0);
    SCENE_STORAGE_RESERVE(l, end, shown, transformf_t, joints == 0 ? 1 : joints);
    SCENE_STORAGE_RESERVE(l, end, frozen, transformf_t, joints == 0 ? 1 : joints);
    SCENE_STORAGE_RESERVE(l, end, model, mat4f_t, joints);
    SCENE_STORAGE_RESERVE(l, end, palette, mat4f_t, joints);
    l.total = end;
    return l;
}

static bool
allocate_animator(scene_animator_t* a, int clips, int curves) {
    const animator_layout_t l = layout_for(clips, curves, a->skeleton.joint_count);
    uint8_t* block = memory_alloc(l.total, MEMORY_PSRAM);
    if (block == NULL) {
        return false;
    }
    memset(block, 0, l.total);
    a->storage = block;
    a->clips = (scene_animator_clip_t*)(void*)(block + l.clips);
    a->counts = (int*)(void*)(block + l.counts);
    a->joint_tables = (const anim_joint_bound_t**)(void*)(block + l.tables);
    a->model = (mat4f_t*)(void*)(block + l.model);
    a->palette = (mat4f_t*)(void*)(block + l.palette);
    const uint8_t joints = a->skeleton.joint_count == 0 ? 1 : a->skeleton.joint_count;
    anim_layer_init(&a->layer, (anim_clip_t*)(void*)(block + l.times), clips,
                    (anim_pose_t){(transformf_t*)(void*)(block + l.shown), joints},
                    (anim_pose_t){(transformf_t*)(void*)(block + l.frozen), joints});
    return true;
}

static asset_status_t
bind_clips(scene_t* scene, scene_animator_t* a, const char* const* ids, int curves, scene_entity_t owner) {
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
        memcpy(name, ids[c], strlen(ids[c]) + 1);
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
            clip->sampled = (anim_bound_t*)(void*)(block + l.sampled) + first;
            status = anim_bind(&clip->tracks, targets.targets, targets.count, clip->fields, a->counts[c], NULL);
            for (int i = 0; status == ANIM_BIND_OK && i < a->counts[c]; i++) {
                clip->sampled[i] = clip->fields[i];
                clip->sampled[i].target = (float*)(void*)(block + l.values) + (first + i) * ANIM_WIDTH_MAX;
                clip->sampled[i].dirty = NULL;
            }
        }
        if (status != ANIM_BIND_OK) {
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

bool
scene_animator_set_clips(scene_t* scene, scene_entity_t entity, const char* skeleton_id, const char* const* ids,
                         int count, scene_failure_t* why) {
    if (entity >= scene->asset.entity_count || count <= 0 || count > ANIM_LAYER_CLIP_NONE) {
        return finish(why, scene_failure_make(SCENE_ERR_ASSET, ASSET_ERR_BOUNDS, scene->id));
    }
    scene_animator_t a = {.root = skeleton_id == NULL ? ANIM_ROOT_SCENE : ANIM_ROOT_SKELETON};
    asset_status_t status = skeleton_id == NULL ? ASSET_OK : anim_skeleton_open(scene->pack, skeleton_id, &a.skeleton);
    const char* failed = skeleton_id == NULL ? ids[0] : skeleton_id;
    int curves = 0;
    if (status == ASSET_OK) {
        status = count_curves(scene, a.root, ids, count, &curves, &failed);
    }
    const scene_skinned_renderer_t* skin = &scene->skinned_renderers[entity];
    if (skin->internal_storage != NULL && skin->skin.joint_count != a.skeleton.joint_count) {
        status = ASSET_ERR_FORMAT;
    }
    if (status != ASSET_OK) {
        return finish(why, scene_failure_make(SCENE_ERR_ASSET, status, failed));
    }
    if (!allocate_animator(&a, count, curves)) {
        return finish(why, scene_failure_make(SCENE_ERR_MEMORY, ASSET_OK, failed));
    }
    status = bind_clips(scene, &a, ids, curves, entity);
    if (status != ASSET_OK) {
        memory_free(a.storage);
        return finish(why, scene_failure_make(SCENE_ERR_ASSET, status, failed));
    }
    if (a.root == ANIM_ROOT_SKELETON) {
        anim_pose_rest(&a.skeleton, &a.layer.shown);
        anim_pose_rest(&a.skeleton, &a.layer.frozen);
        anim_pose_model(&a.skeleton, &a.layer.shown, a.model);
        if (skin->internal_storage != NULL) {
            r3d_skin_palette(&skin->skin, a.model, skin->bind.position_scale, a.palette);
        }
    }
    memory_free(scene->animators[entity].storage);
    scene->animators[entity] = a;
    return finish(why, scene_failure_make(SCENE_OK, ASSET_OK, scene->id));
}

bool
scene_renderer_set_skin(scene_t* scene, scene_entity_t entity, const char* id, scene_failure_t* why) {
    const int renderer = scene_renderer_index(scene, entity);
    if (renderer < 0) {
        return finish(why, scene_failure_make(SCENE_ERR_ASSET, ASSET_ERR_NOT_FOUND, id));
    }
    scene_skinned_renderer_t next = {0};
    asset_status_t status = r3d_skin_open(scene->pack, id, &next.skin);
    const scene_animator_t* animator = &scene->animators[entity];
    const scene_skinned_renderer_t* old = &scene->skinned_renderers[entity];
    next.bind = old->internal_storage == NULL ? scene->renderers[renderer].mesh : old->bind;
    if (status == ASSET_OK
        && (next.skin.joint_count != animator->skeleton.joint_count
            || next.skin.vertex_count != (uint32_t)next.bind.vertex_count)) {
        status = ASSET_ERR_FORMAT;
    }
    if (status != ASSET_OK) {
        return finish(why, scene_failure_make(SCENE_ERR_ASSET, status, id));
    }
    const size_t size = r3d_skin_output_bytes(&next.bind);
    next.internal_storage = memory_alloc(size, MEMORY_INTERNAL);
    if (next.internal_storage == NULL) {
        return finish(why, scene_failure_make(SCENE_ERR_MEMORY, ASSET_OK, id));
    }
    (void)r3d_skin_output_init(&next.bind, next.internal_storage, size, &next.output);
    r3d_skin_palette(&next.skin, animator->model, next.bind.position_scale, animator->palette);
    r3d_skin_clusters(&next.skin, &next.bind, animator->palette, 0, next.bind.cluster_count, &next.output);
    r3d_skin_refit_nodes(&next.output);
    memory_free(scene->skinned_renderers[entity].internal_storage);
    scene->skinned_renderers[entity] = next;
    scene->renderers[renderer].mesh = next.output.mesh;
    return finish(why, scene_failure_make(SCENE_OK, ASSET_OK, id));
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
    return true;
}

static anim_playhead_t*
slot_playhead(scene_animator_t* a, scene_animator_slot_t slot) {
    if (slot == SCENE_ANIMATOR_CURRENT) {
        return &a->layer.current.playhead;
    }
    return slot == SCENE_ANIMATOR_PREVIOUS ? &a->layer.previous.playhead : NULL;
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

const char*
scene_animator_clip_name(const scene_t* scene, scene_entity_t entity, int clip) {
    return clip < 0 || clip >= scene_animator_clip_count(scene, entity) ? NULL
                                                                        : scene->animators[entity].clips[clip].id;
}

void
scene_animation_release(scene_t* scene) {
    for (int e = 0; e < scene->asset.entity_count; e++) {
        memory_free(scene->animators[e].storage);
        memory_free(scene->skinned_renderers[e].internal_storage);
    }
}

void
scene_animate_range(scene_t* scene, int first, int count, uint32_t dt_ms) {
    for (int e = first; e < first + count; e++) {
        scene_animator_t* a = &scene->animators[e];
        if (a->storage == NULL || a->layer.current.clip == ANIM_LAYER_CLIP_NONE) {
            continue;
        }
        anim_layer_advance(&a->layer, dt_ms);
        if (a->root == ANIM_ROOT_SKELETON) {
            const anim_pose_t* pose = anim_layer_pose(&a->layer, &a->skeleton, a->joint_tables, a->counts);
            anim_pose_model(&a->skeleton, pose, a->model);
            const scene_skinned_renderer_t* skin = &scene->skinned_renderers[e];
            if (skin->internal_storage != NULL) {
                r3d_skin_palette(&skin->skin, a->model, skin->bind.position_scale, a->palette);
            }
        } else {
            const anim_slot_t* slot = &a->layer.current;
            const scene_animator_clip_t* clip = &a->clips[slot->clip];
            const float seconds = anim_playhead_seconds(&slot->playhead, &clip->tracks.clip);
            anim_apply(clip->sampled, clip->tracks.count, seconds);
        }
    }
}

static void
apply_property(scene_t* scene, scene_animator_t* a) {
    const scene_animator_clip_t* clip = &a->clips[a->layer.current.clip];
    for (int i = 0; i < clip->tracks.count; i++) {
        const anim_bound_t* target = &clip->fields[i];
        memcpy(target->target, clip->sampled[i].target, target->curve.width * sizeof(float));
        *target->dirty |= target->dirty_bit;
        for (int e = 0; e < scene->asset.entity_count; e++) {
            const uintptr_t value = (uintptr_t)target->target, base = (uintptr_t)&scene->locals[e];
            if (value >= base && value < base + offsetof(transformf_t, matrix)) {
                scene->locals[e].cached = false;
                scene->transforms[e] = r3d_placement_from(&scene->locals[e]);
            }
        }
    }
}

void
scene_skin_range(scene_t* scene, int first, int count) {
    int offset = 0;
    for (int e = 0; e < scene->asset.entity_count; e++) {
        scene_skinned_renderer_t* skin = &scene->skinned_renderers[e];
        if (skin->internal_storage == NULL) {
            continue;
        }
        const int clusters = skin->bind.cluster_count;
        const int start = mathi_clamp(first - offset, 0, clusters),
                  end = mathi_clamp(first + count - offset, 0, clusters);
        r3d_skin_clusters(&skin->skin, &skin->bind, scene->animators[e].palette, start, end - start, &skin->output);
        offset += clusters;
    }
}

typedef struct {
    scene_t* scene;
    int first, count;
    uint32_t dt_ms;
} range_t;

_Static_assert(sizeof(range_t) <= JOB_CTX_MAX, "scene ranges fit job context");

static void
animate_slice(void* ctx) {
    const range_t* r = ctx;
    scene_animate_range(r->scene, r->first, r->count, r->dt_ms);
}

static void
skin_slice(void* ctx) {
    const range_t* r = ctx;
    scene_skin_range(r->scene, r->first, r->count);
}

void
scene_animation_step(scene_t* scene, uint32_t dt_ms) {
    const int entities = scene->asset.entity_count, half = entities / 2;
    range_t a = {scene, 0, half, dt_ms}, b = {scene, half, entities - half, dt_ms};
    job_run_split(animate_slice, &a, &b, sizeof a, true);
    int clusters = 0;
    for (int e = 0; e < entities; e++) {
        scene_animator_t* animator = &scene->animators[e];
        if (animator->storage != NULL && animator->root == ANIM_ROOT_SCENE
            && animator->layer.current.clip != ANIM_LAYER_CLIP_NONE) {
            apply_property(scene, animator);
        }
        clusters +=
            scene->skinned_renderers[e].internal_storage == NULL ? 0 : scene->skinned_renderers[e].bind.cluster_count;
    }
    a = (range_t){scene, 0, clusters / 2, 0};
    b = (range_t){scene, clusters / 2, clusters - (clusters / 2), 0};
    job_run_split(skin_slice, &a, &b, sizeof a, true);
    for (int e = 0; e < entities; e++) {
        if (scene->skinned_renderers[e].internal_storage != NULL) {
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
    p->t_ms = t_ms;
    p->fraction = 0;
    scene_animation_step(scene, 0);
    return true;
}

/* animator: clip binding and playback controls; scene_animation.c advances them each frame. */
#include "scene/animator/animator.h"

#include <assert.h>
#include <stdalign.h>
#include <stdlib.h>
#include <string.h>

#include "anim/anim_tracks_format.h"
#include "core/memory.h"
#include "scene/scene_internal.h"

typedef struct {
    float clip[ANIM_NAME_WIDTH], speed, blend_ms;
} animator_fields_t;

static const anim_field_t ANIMATOR_ANIM_FIELDS[] = {
    [ANIMATOR_FIELD_CLIP] = ANIM_FIELD_STEP(animator_fields_t, clip, ANIM_VALUE_NAME),
    [ANIMATOR_FIELD_SPEED] = ANIM_FIELD_STEP(animator_fields_t, speed, ANIM_VALUE_FLOAT),
    [ANIMATOR_FIELD_BLEND_MS] = ANIM_FIELD_STEP(animator_fields_t, blend_ms, ANIM_VALUE_FLOAT),
};

const anim_component_fields_t ANIMATOR_FIELDS = {
    ANIM_COMPONENT_ANIMATOR,
    ANIMATOR_ANIM_FIELDS,
    sizeof ANIMATOR_ANIM_FIELDS / sizeof ANIMATOR_ANIM_FIELDS[0],
};

_Static_assert(sizeof ANIMATOR_ANIM_FIELDS / sizeof ANIMATOR_ANIM_FIELDS[0] == ANIMATOR_FIELD_COUNT,
               "animator declaration count");

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

static scene_failure_t
claim_replacement(scene_claims_t* claims, const scene_anim_targets_t* targets, const scene_t* scene,
                  const scene_animator_t* a, const char* const* ids, int count, scene_row_t owner,
                  scene_field_mask_t* owned) {
    const scene_animator_t* old = &scene->animators[owner];
    if (old->storage != NULL && old->root == ANIM_ROOT_SCENE) {
        for (int c = 0; c < old->layer.clip_count; c++) {
            const scene_animator_clip_t* clip = &old->clips[c];
            const scene_failure_t result =
                scene_animation_claim_clip(claims, targets, &clip->tracks, clip->id, owner, owned);
            if (result.status != SCENE_OK) {
                return result;
            }
        }
    }
    for (int e = 0; e < scene->asset.entity_count; e++) {
        claims->writes[e] = scene->writes[e] & ~owned[e];
        owned[e] = 0;
    }
    if (a->root == ANIM_ROOT_SCENE) {
        for (int c = 0; c < count; c++) {
            anim_tracks_t tracks;
            (void)anim_tracks_from_pack(scene->pack, ids[c], &tracks);
            const scene_failure_t result = scene_animation_claim_clip(claims, targets, &tracks, ids[c], owner, owned);
            if (result.status != SCENE_OK) {
                return result;
            }
        }
    }
    return scene_failure_make(SCENE_OK, ASSET_OK, scene->root->id);
}

static asset_status_t
bind_animator_storage(scene_t* scene, scene_animator_t* a, const char* const* ids, int curves, scene_row_t owner,
                      const char** failed, const scene_anim_targets_t* targets) {
    const animator_layout_t l = layout_for(a->layer.clip_count, curves, a->skeleton.joint_count);
    uint8_t* block = a->storage;
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
            status =
                anim_bind(&clip->tracks, targets->targets, targets->count, owner, clip->fields, a->counts[c], NULL);
        }
        if (status != ANIM_BIND_OK) {
            *failed = ids[c];
            result = ASSET_ERR_FORMAT;
            break;
        }
        first += a->counts[c];
    }
    return result;
}

/* Keep the old writer claims until every replacement clip has bound. */
static scene_failure_t
bind_clips(scene_t* scene, scene_animator_t* a, const char* const* ids, int count, int curves, scene_row_t row,
           const char* failed, bool replace) {
    const scene_animator_t* old = &scene->animators[row];
    const bool property = a->root == ANIM_ROOT_SCENE;
    const bool needs_claims = replace && (property || (old->storage != NULL && old->root == ANIM_ROOT_SCENE));
    scene_anim_targets_t targets = {0};
    if (property || needs_claims) {
        targets = scene_anim_targets(&scene->asset, scene);
        if (targets.block == NULL) {
            return scene_failure_make(SCENE_ERR_MEMORY, ASSET_OK, failed);
        }
    }
    const size_t bytes = scene->asset.entity_count * sizeof *scene->writes;
    scene_field_mask_t* writes = needs_claims ? memory_alloc(2 * bytes, MEMORY_APP) : NULL;
    scene_failure_t result = scene_failure_make(SCENE_OK, ASSET_OK, failed);
    if (needs_claims) {
        if (writes == NULL) {
            memory_free(targets.block);
            return scene_failure_make(SCENE_ERR_MEMORY, ASSET_OK, failed);
        }
        memset(writes, 0, 2 * bytes);
        scene_claims_t claims = {.asset = &scene->asset, .writes = writes};
        result = claim_replacement(&claims, &targets, scene, a, ids, count, row, writes + scene->asset.entity_count);
    }
    if (result.status == SCENE_OK) {
        if (!allocate_animator(a, count, curves)) {
            result = scene_failure_make(SCENE_ERR_MEMORY, ASSET_OK, failed);
        } else {
            const asset_status_t status = bind_animator_storage(scene, a, ids, curves, row, &failed, &targets);
            if (status != ASSET_OK) {
                scene_free_kept(a->storage);
                result = scene_failure_make(SCENE_ERR_ASSET, status, failed);
            } else if (needs_claims) {
                memcpy(scene->writes, writes, bytes);
            }
        }
    }
    memory_free(writes);
    memory_free(targets.block);
    return result;
}

asset_status_t
scene_animator_read_clip(const asset_pack_t* pack, const char* id, uint8_t root, anim_tracks_t* out) {
    const asset_status_t status = anim_tracks_from_pack(pack, id, out);
    return status == ASSET_OK && out->root != root ? ASSET_ERR_FORMAT : status;
}

static asset_status_t
count_curves(const scene_t* scene, uint8_t root, const char* const* ids, int count, int* curves, const char** failed) {
    for (int c = 0; c < count; c++) {
        anim_tracks_t clip;
        *failed = ids[c];
        const asset_status_t status = scene_animator_read_clip(scene->pack, ids[c], root, &clip);
        if (status != ASSET_OK) {
            return status;
        }
        *curves += clip.count;
    }
    return ASSET_OK;
}

static bool
clip_count_fits(int count, const char* skeleton_id) {
    return count >= 0 && count <= ANIM_LAYER_CLIP_NONE && (count > 0 || skeleton_id != NULL);
}

static scene_animator_t*
animator_of(scene_t* scene, scene_row_t entity) {
    if (entity >= scene->asset.entity_count || scene->animators[entity].storage == NULL) {
        return NULL;
    }
    return &scene->animators[entity];
}

static bool
timeline_owns_clip(const scene_t* scene, scene_row_t row) {
    return scene->timeline != NULL && (scene->writes[row] & SCENE_ANIMATOR_CLIP_BIT);
}

static anim_slot_t*
slot_of(scene_animator_t* a, animator_slot_t slot) {
    if (slot == ANIMATOR_CURRENT) {
        return &a->layer.current;
    }
    return slot == ANIMATOR_PREVIOUS && !a->layer.frozen_previous && a->layer.previous.clip != ANIM_LAYER_CLIP_NONE
               ? &a->layer.previous
               : NULL;
}

/* Setters must charge skipped time at the old playback controls, once. */
static void
scene_animator_settle(scene_animator_t* a) {
    if (a->owed_ms != 0) {
        anim_layer_advance(&a->layer, a->owed_ms);
        a->owed_ms = 0;
        a->dirty = true;
    }
}

static anim_playhead_t*
settled_playhead(entity_t entity, animator_slot_t slot) {
    scene_row_t row;
    scene_t* scene = scene_entity_row(entity, &row);
    scene_animator_t* a = scene == NULL ? NULL : animator_of(scene, row);
    if (a != NULL && timeline_owns_clip(scene, row)) {
        return NULL;
    }
    if (a != NULL) {
        scene_animator_settle(a);
    }
    anim_slot_t* selected = a == NULL ? NULL : slot_of(a, slot);
    return selected == NULL ? NULL : &selected->playhead;
}

static int
clip_count(const scene_t* scene, scene_row_t row) {
    return scene->animators[row].storage == NULL ? 0 : scene->animators[row].layer.clip_count;
}

/* A skeleton animator starts at the rest pose, its skin drawn in it. */
static void
pose_at_rest(scene_animator_t* a, scene_skinned_renderer_t* skin) {
    a->posed = true;
    anim_pose_rest(&a->skeleton, &a->layer.shown);
    anim_pose_rest(&a->skeleton, &a->layer.frozen);
    if (skin->storage != NULL) {
        anim_pose_model(&a->skeleton, &a->layer.shown, skin->model);
        r3d_skin_palette(&skin->skin, skin->model, skin->bind.position_scale, skin->palette);
    }
}

/* Rebinding keeps the slot so copies do not converge onto one pose frame. */
static uint32_t
pose_phase(scene_t* scene, const scene_animator_t* old, uint8_t root) {
    if (old->storage != NULL && old->root == ANIM_ROOT_SKELETON) {
        return old->phase;
    }
    return root == ANIM_ROOT_SKELETON ? scene->animation_phase++ : 0;
}

bool
scene_animator_set_clips(scene_t* scene, scene_row_t row, const char* skeleton_id, const char* const* ids, int count,
                         bool replace, scene_failure_t* why) {
    if (row >= scene->asset.entity_count || !clip_count_fits(count, skeleton_id)) {
        return scene_report(why, SCENE_ERR_ASSET, ASSET_ERR_BOUNDS, scene->root->id);
    }
    if (replace && timeline_owns_clip(scene, row)) {
        return scene_report(why, SCENE_ERR_ASSET, ASSET_ERR_FORMAT, scene->asset.names[row].name);
    }
    scene_animator_t a = {.root = skeleton_id == NULL ? ANIM_ROOT_SCENE : ANIM_ROOT_SKELETON, .wrap = ANIM_LOOP};
    asset_status_t status = skeleton_id == NULL ? ASSET_OK : anim_skeleton_open(scene->pack, skeleton_id, &a.skeleton);
    const char* failed = skeleton_id == NULL ? ids[0] : skeleton_id;
    int curves = 0;
    if (status == ASSET_OK) {
        status = count_curves(scene, a.root, ids, count, &curves, &failed);
    }
    scene_skinned_renderer_t* skin = &scene->skinned_renderers[row];
    if (skin->bound && skin->skin.joint_count != a.skeleton.joint_count) {
        status = ASSET_ERR_FORMAT;
    }
    if (status != ASSET_OK) {
        return scene_report(why, SCENE_ERR_ASSET, status, failed);
    }
    const scene_animator_t* old = &scene->animators[row];
    const scene_failure_t bound = bind_clips(scene, &a, ids, count, curves, row, failed, replace);
    if (bound.status != SCENE_OK) {
        if (why != NULL) {
            *why = bound;
        }
        return false;
    }
    if (a.root == ANIM_ROOT_SKELETON) {
        pose_at_rest(&a, skin);
    }
    scene->skeleton_animator_count +=
        (a.root == ANIM_ROOT_SKELETON) - (old->storage != NULL && old->root == ANIM_ROOT_SKELETON);
    scene->animator_count += old->storage == NULL;
    /* A taken block cannot be freed: rebinding a scene's clips abandons the
     * old storage to the lender until the scene's memory goes back. */
    scene_free_kept(old->storage);
    a.phase = pose_phase(scene, old, a.root);
    a.dirty = true;
    scene->animators[row] = a;
    scene_stage_add(scene, row);
    return scene_report(why, SCENE_OK, ASSET_OK, scene->root->id);
}

bool
animator_set_clips(entity_t entity, const char* skeleton_id, const char* const* ids, int count, scene_failure_t* why) {
    scene_row_t row;
    scene_t* scene = scene_entity_row(entity, &row);
    if (scene == NULL) {
        return scene_report(why, SCENE_ERR_ASSET, ASSET_ERR_NOT_FOUND, "");
    }
    return scene_animator_set_clips(scene, row, skeleton_id, ids, count, true, why);
}

bool
animator_play(entity_t entity, int clip, uint32_t blend_ms) {
    scene_row_t row;
    scene_t* scene = scene_entity_row(entity, &row);
    if (scene == NULL) {
        return false;
    }
    scene_animator_t* a = animator_of(scene, row);
    if (a == NULL || timeline_owns_clip(scene, row) || clip < 0 || clip >= a->layer.clip_count
        || (a->root == ANIM_ROOT_SCENE && blend_ms != 0)) {
        return false;
    }
    scene_animator_settle(a);
    anim_layer_play(&a->layer, (uint8_t)clip, blend_ms);
    a->layer.current.playhead.wrap = a->wrap;
    a->dirty = true;
    return true;
}

bool
animator_set_speed(entity_t entity, animator_slot_t slot, float speed) {
    anim_playhead_t* p = settled_playhead(entity, slot);
    return p != NULL && anim_playhead_set_speed(p, speed);
}

bool
animator_set_paused(entity_t entity, animator_slot_t slot, bool paused) {
    anim_playhead_t* p = settled_playhead(entity, slot);
    if (p == NULL) {
        return false;
    }
    p->paused = paused;
    return true;
}

bool
animator_paused(entity_t entity, animator_slot_t slot) {
    scene_row_t row;
    scene_t* scene = scene_entity_row(entity, &row);
    scene_animator_t* a = scene == NULL ? NULL : animator_of(scene, row);
    const anim_slot_t* selected = a == NULL ? NULL : slot_of(a, slot);
    return selected != NULL && selected->playhead.paused;
}

int
animator_clip_count(entity_t entity) {
    scene_row_t row;
    const scene_t* scene = scene_entity_row(entity, &row);
    if (scene == NULL) {
        return 0;
    }
    return row >= scene->asset.entity_count || scene->animators[row].storage == NULL
               ? 0
               : scene->animators[row].layer.clip_count;
}

int
animator_current_clip(entity_t entity) {
    scene_row_t row;
    const scene_t* scene = scene_entity_row(entity, &row);
    if (scene == NULL) {
        return -1;
    }
    return clip_count(scene, row) == 0 || scene->animators[row].layer.current.clip == ANIM_LAYER_CLIP_NONE
               ? -1
               : scene->animators[row].layer.current.clip;
}

const char*
animator_clip_name(entity_t entity, int clip) {
    scene_row_t row;
    const scene_t* scene = scene_entity_row(entity, &row);
    if (scene == NULL) {
        return NULL;
    }
    return clip < 0 || clip >= clip_count(scene, row) ? NULL : scene->animators[row].clips[clip].id;
}

uint32_t
animator_clip_duration_ms(entity_t entity, int clip) {
    scene_row_t row;
    const scene_t* scene = scene_entity_row(entity, &row);
    if (scene == NULL) {
        return 0;
    }
    return clip < 0 || clip >= clip_count(scene, row) ? 0 : scene->animators[row].clips[clip].tracks.clip.duration_ms;
}

float
animator_clip_ground_mps(entity_t entity, int clip) {
    scene_row_t row;
    const scene_t* scene = scene_entity_row(entity, &row);
    if (scene == NULL || clip < 0 || clip >= clip_count(scene, row)) {
        return 0.0F;
    }
    return (float)scene->animators[row].clips[clip].tracks.clip.ground_mm_s / (float)ANIM_TRACKS_GROUND_PER_M;
}

bool
animator_seek(entity_t entity, animator_slot_t slot, uint32_t t_ms) {
    scene_row_t row;
    scene_t* scene = scene_entity_row(entity, &row);
    if (scene == NULL) {
        return false;
    }
    scene_animator_t* a = animator_of(scene, row);
    anim_slot_t* selected = a == NULL ? NULL : slot_of(a, slot);
    if (selected == NULL || timeline_owns_clip(scene, row) || selected->clip == ANIM_LAYER_CLIP_NONE) {
        return false;
    }
    scene_animator_settle(a);
    if (selected->clip == ANIM_LAYER_CLIP_NONE) {
        return false;
    }
    scene_draw_camera_cut(scene, row);
    anim_playhead_seek(&selected->playhead, &a->layer.clips[selected->clip], t_ms);
    a->dirty = true;
    scene_animation_repose(scene, 0);
    return true;
}

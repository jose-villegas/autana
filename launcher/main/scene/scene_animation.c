/* Scene animation owns component bindings and buffers; entity and cluster jobs synchronize before bounds refit. */
#include <assert.h>
#include <stdalign.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "anim/anim_tracks_format.h"
#include "core/job.h"
#include "core/memory.h"
#include "math/scalar/mathi.h"
#include "profile/frame_cost.h"
#include "render/context/render_context.h"
#include "render/record/frame_record.h"
#include "scene/scene_anim_stages.h"
#include "scene/scene_internal.h"

scene_anim_targets_t
scene_anim_targets(scene_t* scene, scene_row_t owner) {
    enum { SCENE_ANIM_COMPONENTS_MAX = 2 };

    const int entities = scene->asset.entity_count;
    size_t end = 0;
    const size_t targets = mem_carve(&end, sizeof(anim_target_t) * (size_t)entities, alignof(anim_target_t));
    const size_t components =
        mem_carve(&end, sizeof(anim_component_ref_t) * (size_t)entities * SCENE_ANIM_COMPONENTS_MAX,
                  alignof(anim_component_ref_t));
    uint8_t* block = memory_alloc(end, MEMORY_APP);
    scene_anim_targets_t out = {.block = block, .count = entities};
    if (block == NULL) {
        return out;
    }
    out.targets = (anim_target_t*)(void*)(block + targets);
    anim_component_ref_t* refs = (anim_component_ref_t*)(void*)(block + components);
    for (int e = 0; e < entities; e++) {
        anim_component_ref_t* fields = refs + (e * SCENE_ANIM_COMPONENTS_MAX);
        fields[0] = (anim_component_ref_t){&ANIM_POSE_TRANSFORM_FIELDS, &scene->locals[e]};
        out.targets[e] = (anim_target_t){scene->asset.names[e].hash, fields, 1, &scene->flags[e], SCENE_FLAG_CHANGED};
    }
    for (int c = 0; c < scene->asset.camera_count; c++) {
        const int e = scene->cameras[c].entity;
        anim_target_t* target = &out.targets[e];
        const int slot = SCENE_ANIM_COMPONENTS_MAX - 1;
        assert(slot < SCENE_ANIM_COMPONENTS_MAX && slot <= target->component_count);
        refs[(e * SCENE_ANIM_COMPONENTS_MAX) + slot] = (anim_component_ref_t){&LENS_FIELDS, &scene->cameras[c].lens};
        target->component_count = slot + 1;
    }
    if (owner != SCENE_ROW_NONE) {
        const anim_target_t first = out.targets[0];
        out.targets[0] = out.targets[owner];
        out.targets[owner] = first;
    }
    return out;
}

/* The pose interval of tier `tier` under `s`. */
static int
tier_every(int tier, const render_anim_settings_t* s) {
    const int every[] = {[SCENE_ANIM_TIER_NEAR] = RENDER_ANIM_EVERY_MIN,
                         [SCENE_ANIM_TIER_MID] = s->mid_every,
                         [SCENE_ANIM_TIER_FAR] = s->far_every};
    return every[tier];
}

/* Keep a tier inside its band so camera jitter does not alternate pose rates:
 * each boundary moves away from the current tier by the band. */
static int
animation_interval(scene_animator_t* a, const render_anim_settings_t* s) {
    const float band = a->interval == 0 ? 0.0F : s->hysteresis_px;
    const float mid = a->tier == SCENE_ANIM_TIER_NEAR ? s->mid_px - band : s->mid_px + band;
    const float far = a->tier == SCENE_ANIM_TIER_FAR ? s->far_px + band : s->far_px - band;
    const float px = a->drawn_radius_px;
    a->tier = px < far ? SCENE_ANIM_TIER_FAR : px < mid ? SCENE_ANIM_TIER_MID : SCENE_ANIM_TIER_NEAR;
    a->interval = tier_every(a->tier, s);
    return a->interval;
}

void
scene_animation_release(scene_t* scene) {
    for (int e = 0; e < scene->asset.entity_count; e++) {
        scene_free_kept(scene->animators[e].storage);
        scene_skin_give(scene, &scene->skinned_renderers[e]);
        scene->animators[e] = (scene_animator_t){0};
        scene->skinned_renderers[e] = (scene_skinned_renderer_t){0};
    }
    scene->animator_count = 0;
    scene->skeleton_animator_count = 0;
}

void
scene_stage_add(scene_t* scene, scene_row_t entity) {
    int at = scene->staged_count;
    while (at > 0 && scene->staged[at - 1] >= entity) {
        if (scene->staged[--at] == entity) {
            return;
        }
    }
    memmove(&scene->staged[at + 1], &scene->staged[at], sizeof *scene->staged * (size_t)(scene->staged_count - at));
    scene->staged[at] = entity;
    scene->staged_count++;
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

/* A copy whose clip poses it on its tier's slots: its move relights it there. */
static bool
paced(const scene_t* scene, scene_row_t e) {
    const scene_animator_t* a = &scene->animators[e];
    return a->storage != NULL && a->root == ANIM_ROOT_SKELETON && a->layer.current.clip != ANIM_LAYER_CLIP_NONE
           && (scene->flags[e] & SCENE_FLAG_ACTIVE);
}

/* Whether copy `e` poses this step. A bound copy's tier changes only on the
 * frames the draw measured it for, and its interval follows the settings
 * every step; a copy with no skin bound poses every step. */
static bool
pose_due(const scene_t* scene, scene_row_t e, const render_anim_settings_t* settings) {
    if (!scene->skinned_renderers[e].bound) {
        return true;
    }
    scene_animator_t* a = &scene->animators[e];
    a->interval = scene_animator_due(scene, a) ? animation_interval(a, settings) : tier_every(a->tier, settings);
    return scene_animator_due(scene, a);
}

void
scene_animate_range(scene_t* scene, int first, int count, uint32_t dt_ms, const render_anim_settings_t* settings) {
    for (int i = first; i < first + count; i++) {
        const scene_row_t e = scene->staged[i];
        scene_animator_t* a = &scene->animators[e];
        a->posed = false;
        if (!paced(scene, e)) {
            continue;
        }
        const bool force = a->dirty;
        const bool due = pose_due(scene, e, settings);
        /* A zero-time repose belongs to its dirty copy; others keep their debt. */
        if (!force && (dt_ms == 0 || !due)) {
            a->owed_ms += dt_ms;
            continue;
        }
        const bool advanced = advance_animator(a, dt_ms + a->owed_ms);
        a->owed_ms = 0;
        a->posed = advanced || (scene->flags[e] & SCENE_FLAG_RELIGHT);
        if (!advanced) {
            continue;
        }
        const anim_pose_t* pose = anim_layer_pose(&a->layer, &a->skeleton, a->joint_tables, a->counts);
        scene_skinned_renderer_t* skin = &scene->skinned_renderers[e];
        if (skin->storage != NULL) {
            anim_pose_model(&a->skeleton, pose, skin->model);
            r3d_skin_palette(&skin->skin, skin->model, skin->bind.position_scale, skin->palette);
        }
    }
}

bool
scene_field_claim(scene_t* scene, scene_row_t entity, scene_field_bit_t field) {
    assert(entity < scene->asset.entity_count);
    if (scene->writes[entity] & field) {
        return false;
    }
    scene->writes[entity] |= (uint16_t)field;
    return true;
}

static scene_row_t
binding_entity(const scene_asset_t* asset, const anim_binding_t* binding, scene_row_t owner) {
    if (strcmp(binding->path, ANIM_TARGET_SELF) == 0) {
        return owner;
    }
    for (scene_row_t i = 0; i < asset->entity_count; i++) {
        scene_row_t entity = i;
        if (owner != SCENE_ROW_NONE) {
            entity = i == 0 ? owner : i == owner ? 0 : i;
        }
        if (asset->names[entity].hash == binding->path_hash) {
            return entity;
        }
    }
    return SCENE_ROW_NONE;
}

static scene_field_bit_t
binding_bit(const scene_asset_t* asset, scene_row_t entity, const anim_binding_t* binding) {
    transformf_t transform;
    lens_t lens;
    const anim_component_ref_t components[] = {{&ANIM_POSE_TRANSFORM_FIELDS, &transform}, {&LENS_FIELDS, &lens}};
    int count = 1;
    for (int c = 0; c < asset->camera_count; c++) {
        if (asset->cameras[c].entity == entity) {
            count = sizeof components / sizeof *components;
            break;
        }
    }
    anim_bound_t bound;
    if (anim_bind_field(binding, components, count, NULL, 0, &bound) != ANIM_BIND_OK) {
        return 0;
    }

    const struct {
        void* target;
        scene_field_bit_t bit;
    } fields[] = {{&transform.position, SCENE_FIELD_POSITION},
                  {&transform.rotation, SCENE_FIELD_ROTATION},
                  {&transform.scale, SCENE_FIELD_SCALE},
                  {&lens.half_fov_short_tan, SCENE_FIELD_HALF_FOV_SHORT_TAN},
                  {&lens.near_z, SCENE_FIELD_NEAR_Z}};

    for (size_t f = 0; f < sizeof fields / sizeof *fields; f++) {
        if (fields[f].target == bound.target) {
            return fields[f].bit;
        }
    }
    return 0;
}

static bool
lens_keys_fit(const anim_track_t* curve) {
    for (int key = 0; key < curve->count; key++) {
        float value[ANIM_WIDTH_MAX];
        anim_track_sample(curve, curve->times[key], value);
        if (!lens_value_fits(value[0])) {
            return false;
        }
    }
    return true;
}

scene_failure_t
scene_animation_claim_clip(scene_t* scene, const anim_tracks_t* tracks, const char* clip, scene_row_t owner,
                           uint16_t* owned) {
    for (int i = 0; i < tracks->count; i++) {
        anim_binding_t binding;
        (void)anim_tracks_binding_at(tracks, i, &binding);
        const scene_row_t entity = binding_entity(&scene->asset, &binding, owner);
        const scene_field_bit_t bit = entity == SCENE_ROW_NONE ? 0 : binding_bit(&scene->asset, entity, &binding);
        if (bit == 0) {
            return scene_failure_make(SCENE_ERR_ASSET, ASSET_ERR_FORMAT, clip);
        }
        if ((binding.component == ANIM_COMPONENT_CAMERA && !lens_keys_fit(&binding.curve))
            || ((owned == NULL || !(owned[entity] & bit)) && !scene_field_claim(scene, entity, bit))) {
            scene_failure_t failure = {SCENE_ERR_ASSET, ASSET_ERR_FORMAT, ""};
            (void)snprintf(failure.what, sizeof failure.what, "%s.%s", scene->asset.names[entity].name, binding.field);
            return failure;
        }
        if (owned != NULL) {
            owned[entity] |= (uint16_t)bit;
        }
    }
    return scene_failure_make(SCENE_OK, ASSET_OK, clip);
}

void
scene_playhead_seek(anim_playhead_t* playhead, uint32_t t_ms) {
    playhead->t_ms = t_ms;
    playhead->fraction = 0;
}

void
scene_animation_apply_fields(scene_t* scene, const anim_bound_t* fields, int count, float seconds) {
    anim_apply(fields, count, seconds);
    for (int i = 0; i < count; i++) {
        const ptrdiff_t entity = fields[i].dirty - scene->flags;
        scene->locals[entity].cached = false;
    }
}

static void
apply_property(scene_t* scene, scene_animator_t* a, uint32_t dt_ms) {
    if (!advance_animator(a, dt_ms)) {
        return;
    }
    const anim_slot_t* slot = &a->layer.current;
    const scene_animator_clip_t* clip = &a->clips[slot->clip];
    scene_animation_apply_fields(scene, clip->fields, clip->tracks.count, anim_playhead_seconds(&slot->playhead));
}

static const r3d_skin_light_kernel_t LIGHT_KERNELS[] = {
    [SKIN_LIGHT_DIRECT] = r3d_skin_clusters_lit,
    [SKIN_LIGHT_RENORMALISED] = r3d_skin_clusters_renormalised,
    [SKIN_LIGHT_TABLE] = r3d_skin_clusters_table,
};
_Static_assert(sizeof LIGHT_KERNELS / sizeof LIGHT_KERNELS[0] == SKIN_LIGHT_TABLE + 1, "lighting mode count");

void
scene_skin_range(scene_t* scene, int first, int count) {
    int offset = 0;
    for (int i = 0; i < scene->staged_count; i++) {
        const scene_row_t e = scene->staged[i];
        scene_skinned_renderer_t* skin = &scene->skinned_renderers[e];
        if (skin->storage == NULL || !scene->animators[e].posed) {
            continue;
        }
        const int clusters = skin->bind.cluster_count;
        const int start = mathi_clamp(first - offset, 0, clusters);
        const int end = mathi_clamp(first + count - offset, 0, clusters);
        if (scene->light_transfer != NULL && skin->lighting_valid) {
            if (scene->probes != NULL) {
                const int r = scene_renderer_index(scene, e);
                const sh1_rgb_t radiance =
                    sh_probes_sample(scene->probes, box3f_centre(scene->renderers[r].world_bounds));
                const sh1_irradiance_t ambient = r3d_skin_light_ambient_sh(&radiance);
                r3d_skin_clusters_ambient(&skin->skin, &skin->bind, skin->palette, start, end - start, &skin->output,
                                          skin->normal_palette, &scene->world_lights, skin->colors, &ambient,
                                          skin->light_mode);
            } else {
                LIGHT_KERNELS[skin->light_mode](&skin->skin, &skin->bind, skin->palette, start, end - start,
                                                &skin->output, skin->normal_palette, &scene->world_lights,
                                                skin->colors);
            }
        } else {
            r3d_skin_clusters(&skin->skin, &skin->bind, skin->palette, start, end - start, &skin->output);
        }
        if (start == 0 && end > 0) { /* the range from the first cluster tells the recorder once per pose */
            frame_record_note_skin(&skin->output.mesh, &skin->bind, &skin->skin, skin->palette, skin->skin.joint_count);
        }
        offset += clusters;
    }
}

typedef struct {
    scene_t* scene;
    uint32_t dt_ms;
    render_anim_settings_t settings;
} animate_work_t;

static void
animate_slice(void* ctx, int from, int to) {
    const animate_work_t* w = ctx;
    scene_animate_range(w->scene, from, to - from, w->dt_ms, &w->settings);
}

static void
skin_slice(void* ctx, int from, int to) {
    scene_skin_range(ctx, from, to - from);
}

/* The tiers resolve under the overrides before the parallel slices, so every
 * copy sees one policy; in place, so no second copy shares the repose's frame,
 * which the skin and draw slices run beneath. */
static void
animate_entities(scene_t* scene, uint32_t dt_ms) {
    if (scene->skeleton_animator_count == 0) {
        return;
    }
    animate_work_t w = {scene, dt_ms, render_context_settings(render_context_main())->anim};
    render_anim_settings_tune(&w.settings);
    (void)job_run_range(&scene->animate_share, animate_slice, &w, scene->staged_count);
}

static bool
skin_lights_changed(const scene_t* scene) {
    if (scene->light_count == 0) {
        return false;
    }
    bool lights_dirty = false;
    for (int i = 0; i < scene->staged_count; i++) {
        const scene_row_t e = scene->staged[i];
        lights_dirty |= scene->lights[e].kind != SCENE_LIGHT_NONE
                        && (scene->flags[e] & (SCENE_FLAG_SKIN_DIRTY | SCENE_FLAG_RELIGHT));
    }
    return lights_dirty;
}

static int
skin_stage_prepare(scene_t* scene) {
    const int entities = scene->asset.entity_count;
    int clusters = 0;
    const bool lights_dirty = skin_lights_changed(scene);
    scene->light_table_dirty |= lights_dirty;
    for (int i = 0; i < scene->staged_count; i++) {
        const scene_row_t e = scene->staged[i];
        scene_animator_t* animator = &scene->animators[e];
        scene_skinned_renderer_t* skin = &scene->skinned_renderers[e];
        if (!(scene->flags[e] & SCENE_FLAG_ACTIVE)) {
            continue;
        }
        if (skin->storage != NULL && scene->light_transfer != NULL) {
            /* A paused clip reaches no slot, so its copy relights at once, as one with no clip. */
            const bool moved_unpaced =
                (scene->flags[e] & SCENE_FLAG_RELIGHT) && (!paced(scene, e) || animator->layer.current.playhead.paused);
            animator->posed |= lights_dirty || (scene->flags[e] & SCENE_FLAG_SKIN_DIRTY) || moved_unpaced;
        }
        if (skin->storage != NULL && animator->posed) {
            scene_skin_light_prepare(scene, e);
            clusters += skin->bind.cluster_count;
        }
    }
    for (int e = 0; e < entities; e++) {
        /* A lit copy not relit this step keeps its move for its next pose. */
        const bool waits =
            scene->skinned_renderers[e].storage != NULL && scene->light_transfer != NULL && !scene->animators[e].posed;
        scene->flags[e] &= (uint8_t)~(SCENE_FLAG_SKIN_DIRTY | (waits ? 0U : SCENE_FLAG_RELIGHT));
    }
    return clusters;
}

void
scene_animation_repose(scene_t* scene, uint32_t dt_ms) {
    if (scene->animator_count == 0) {
        return;
    }
    FRAME_COST_BEGIN(posed_from);
    for (int i = 0; i < scene->staged_count; i++) {
        const scene_row_t e = scene->staged[i];
        scene_animator_t* animator = &scene->animators[e];
        if (animator->storage != NULL && animator->root == ANIM_ROOT_SCENE
            && animator->layer.current.clip != ANIM_LAYER_CLIP_NONE && (scene->flags[e] & SCENE_FLAG_ACTIVE)) {
            apply_property(scene, animator, dt_ms);
        }
    }
    animate_entities(scene, dt_ms);
    FRAME_COST_END(posed_from, SCENE_ANIM_STAGE_POSE_NAME);
    FRAME_COST_BEGIN(lit_from);
    const int clusters = skin_stage_prepare(scene);
    if (clusters > 0 && scene->light_transfer != NULL) {
        scene_lights_prepare(scene);
    }
    FRAME_COST_END(lit_from, SCENE_ANIM_STAGE_LIGHT_NAME);
    if (clusters == 0) {
        return;
    }
    FRAME_COST_BEGIN(skinned_from);
    (void)job_run_range(&scene->skin_share, skin_slice, scene, clusters);
    FRAME_COST_END(skinned_from, SCENE_ANIM_STAGE_SKIN_NAME);
    FRAME_COST_BEGIN(refit_from);
    for (int i = 0; i < scene->staged_count; i++) {
        const scene_row_t e = scene->staged[i];
        if (scene->skinned_renderers[e].storage != NULL && scene->animators[e].posed) {
            r3d_skin_refit_nodes(&scene->skinned_renderers[e].output);
            scene_renderer_bound(&scene->renderers[scene_renderer_index(scene, e)]);
        }
    }
    FRAME_COST_END(refit_from, SCENE_ANIM_STAGE_REFIT_NAME);
}

void
scene_animation_step(scene_t* scene, uint32_t dt_ms) {
    scene_skins_settle(scene);
    scene_timeline_step(scene, dt_ms);
    scene_animation_repose(scene, dt_ms);
    scene->animation_frame++;
    /* Only a draw between steps can supply the next step's visible size. */
    for (int i = 0; i < scene->staged_count; i++) {
        scene->animators[scene->staged[i]].drawn_radius_px = 0.0F;
    }
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
            skin->storage == NULL ? 0 : (size_t)((uint8_t*)(skin->palette + joints) - (uint8_t*)skin->storage);
        const size_t light_bytes =
            skin->light_storage == NULL
                ? 0
                : (size_t)((uint8_t*)(skin->normal_palette + joints) - (uint8_t*)skin->light_storage);
        checkpoint_capture_at(saved, &skin->storage, internal_bytes);
        checkpoint_capture_at(saved, &skin->light_storage, light_bytes);
    }
}
#endif

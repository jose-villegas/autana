/* Scene animation owns component bindings and buffers; entity and cluster jobs synchronize before bounds refit. */
#include <assert.h>
#include <stdalign.h>
#include <stdlib.h>
#include <string.h>
#include "anim/anim_tracks_format.h"
#include "core/job.h"
#include "core/memory.h"
#include "core/tune.h"
#include "math/scalar/mathi.h"
#include "render/context/render_context.h"
#include "scene/scene_internal.h"

TUNE_OWNER(anim);
TUNE_OVERRIDE(anim, mid_px, RENDER_ANIM_PIXELS_MAX);
TUNE_OVERRIDE(anim, far_px, RENDER_ANIM_PIXELS_MAX);
TUNE_OVERRIDE(anim, mid_every, RENDER_ANIM_EVERY_MAX);
TUNE_OVERRIDE(anim, far_every, RENDER_ANIM_EVERY_MAX);
TUNE_OVERRIDE(anim, hysteresis_px, RENDER_ANIM_PIXELS_MAX);

/* Resolve overrides before the parallel slices so every copy sees one policy. */
static render_anim_settings_t
animation_settings(void) {
    render_anim_settings_t s = render_context_settings(render_context_main())->anim;
    s.mid_px = mid_px == TUNE_UNSET ? s.mid_px : (float)mathi_clamp(mid_px, 0, RENDER_ANIM_PIXELS_MAX);
    s.far_px = far_px == TUNE_UNSET ? s.far_px : (float)mathi_clamp(far_px, 0, RENDER_ANIM_PIXELS_MAX);
    s.hysteresis_px =
        hysteresis_px == TUNE_UNSET ? s.hysteresis_px : (float)mathi_clamp(hysteresis_px, 0, RENDER_ANIM_PIXELS_MAX);
    s.mid_every = mathi_clamp(tune_override(mid_every, s.mid_every), RENDER_ANIM_EVERY_MIN, RENDER_ANIM_EVERY_MAX);
    s.far_every = mathi_clamp(tune_override(far_every, s.far_every), RENDER_ANIM_EVERY_MIN, RENDER_ANIM_EVERY_MAX);
    return s;
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
    const int every[] = {[SCENE_ANIM_TIER_NEAR] = RENDER_ANIM_EVERY_MIN,
                         [SCENE_ANIM_TIER_MID] = s->mid_every,
                         [SCENE_ANIM_TIER_FAR] = s->far_every};
    a->interval = every[a->tier];
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

void
scene_animate_range(scene_t* scene, int first, int count, uint32_t dt_ms, const render_anim_settings_t* settings) {
    for (int i = first; i < first + count; i++) {
        const scene_row_t e = scene->staged[i];
        scene_animator_t* a = &scene->animators[e];
        a->posed = false;
        if (!paced(scene, e)) {
            continue;
        }
        const int interval =
            scene->skinned_renderers[e].bound ? animation_interval(a, settings) : RENDER_ANIM_EVERY_MIN;
        const bool force = a->dirty;
        const bool due = (scene->animation_frame + a->phase) % (uint32_t)interval == 0;
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

static void
animate_entities(scene_t* scene, uint32_t dt_ms, render_anim_settings_t settings) {
    if (scene->skeleton_animator_count == 0) {
        return;
    }
    animate_work_t w = {scene, dt_ms, settings};
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
    for (int i = 0; i < scene->staged_count; i++) {
        const scene_row_t e = scene->staged[i];
        scene_animator_t* animator = &scene->animators[e];
        if (animator->storage != NULL && animator->root == ANIM_ROOT_SCENE
            && animator->layer.current.clip != ANIM_LAYER_CLIP_NONE && (scene->flags[e] & SCENE_FLAG_ACTIVE)) {
            apply_property(scene, animator, dt_ms);
        }
    }
    animate_entities(scene, dt_ms, animation_settings());
    const int clusters = skin_stage_prepare(scene);
    if (clusters == 0) {
        return;
    }
    if (scene->light_transfer != NULL) {
        scene_lights_prepare(scene);
    }
    (void)job_run_range(&scene->skin_share, skin_slice, scene, clusters);
    for (int i = 0; i < scene->staged_count; i++) {
        const scene_row_t e = scene->staged[i];
        if (scene->skinned_renderers[e].storage != NULL && scene->animators[e].posed) {
            r3d_skin_refit_nodes(&scene->skinned_renderers[e].output);
            scene_renderer_bound(&scene->renderers[scene_renderer_index(scene, e)]);
        }
    }
}

void
scene_animation_step(scene_t* scene, uint32_t dt_ms) {
    scene_skins_settle(scene);
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

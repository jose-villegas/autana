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

void
scene_animate_range(scene_t* scene, int first, int count, uint32_t dt_ms) {
    for (int i = first; i < first + count; i++) {
        const scene_row_t e = scene->staged[i];
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
        lights_dirty |= scene->lights[e].kind != SCENE_LIGHT_NONE && (scene->flags[e] & SCENE_FLAG_SKIN_DIRTY);
    }
    return lights_dirty;
}

static int
skin_stage_prepare(scene_t* scene, uint32_t dt_ms) {
    const int entities = scene->asset.entity_count;
    int clusters = 0;
    for (int i = 0; i < scene->staged_count; i++) {
        const scene_row_t e = scene->staged[i];
        scene_animator_t* animator = &scene->animators[e];
        if (animator->storage != NULL && animator->root == ANIM_ROOT_SCENE
            && animator->layer.current.clip != ANIM_LAYER_CLIP_NONE && (scene->flags[e] & SCENE_FLAG_ACTIVE)) {
            apply_property(scene, animator, dt_ms);
        }
    }
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
            animator->posed |= lights_dirty || (scene->flags[e] & SCENE_FLAG_SKIN_DIRTY);
        }
        if (skin->storage != NULL && animator->posed) {
            scene_skin_light_prepare(scene, e);
            clusters += skin->bind.cluster_count;
        }
    }
    for (int e = 0; e < entities; e++) {
        scene->flags[e] &= (uint8_t)~SCENE_FLAG_SKIN_DIRTY;
    }
    return clusters;
}

void
scene_animation_repose(scene_t* scene, uint32_t dt_ms) {
    if (scene->animator_count == 0) {
        return;
    }
    animate_entities(scene, dt_ms);
    const int clusters = skin_stage_prepare(scene, dt_ms);
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

/* timeline: binds and samples the clip on a scene root's millisecond clock. */
#include "scene/timeline/timeline.h"

#include <math.h>

#include "scene/scene_internal.h"

static scene_t*
timeline_scene(entity_t root) {
    scene_row_t row;
    scene_root_t* record = scene_entity_lookup(root, &row);
    return record != NULL && row == SCENE_ROW_ROOT && record->scene != NULL && record->scene->timeline != NULL
               ? record->scene
               : NULL;
}

static void
timeline_camera_cut(const scene_t* scene) {
    const scene_timeline_t* timeline = scene->timeline;
    for (int i = 0; i < timeline->tracks.count; i++) {
        scene_draw_camera_cut(scene, (scene_row_t)(timeline->fields[i].dirty - scene->flags));
    }
}

void
scene_timeline_step(scene_t* scene, uint32_t dt_ms) {
    scene_timeline_t* timeline = scene->timeline;
    if (timeline == NULL) {
        return;
    }
    anim_playhead_t* clock = &timeline->clock;
    const uint32_t duration = timeline->tracks.clip.duration_ms;
    const bool looping = clock->wrap == ANIM_LOOP && duration != 0 && !clock->paused;
    const uint64_t advance = looping ? (uint64_t)dt_ms * clock->speed_q16 + clock->fraction : 0;
    const uint64_t remaining = (uint64_t)(duration - clock->t_ms) << ANIM_PLAYHEAD_SPEED_SHIFT;
    anim_playhead_advance(clock, &timeline->tracks.clip, dt_ms);
    const uint32_t now = timeline->clock.t_ms;
    if (timeline->dirty || now != timeline->applied_ms) {
        /* A rounded duration can precede the last key by a fraction of a millisecond. */
        const float seconds = duration != 0 && now == duration ? INFINITY : anim_playhead_seconds(&timeline->clock);
        scene_animation_apply_fields(scene, timeline->fields, timeline->tracks.count, seconds);
        timeline->dirty = false;
        timeline->applied_ms = now;
    }
    if (looping && advance >= remaining) {
        timeline_camera_cut(scene);
    }
}

scene_failure_t
scene_timeline_bind(scene_t* scene) {
    scene_timeline_t* timeline = scene->timeline;
    const char* clip = scene->asset.timeline->clip;
    const scene_failure_t claims = scene_animation_claim_clip(scene, &timeline->tracks, clip, SCENE_ROW_NONE, NULL);
    if (claims.status != SCENE_OK) {
        return claims;
    }
    if (timeline->tracks.count != 0) {
        const scene_anim_targets_t targets = scene_anim_targets(scene, SCENE_ROW_NONE);
        if (targets.block == NULL) {
            return scene_failure_make(SCENE_ERR_MEMORY, ASSET_OK, clip);
        }
        const anim_bind_status_t status = anim_bind(&timeline->tracks, targets.targets, targets.count, timeline->fields,
                                                    timeline->tracks.count, NULL);
        memory_free(targets.block);
        if (status != ANIM_BIND_OK) {
            return scene_failure_make(SCENE_ERR_ASSET, ASSET_ERR_FORMAT, clip);
        }
    }
    timeline->dirty = true;
    scene_timeline_step(scene, 0);
    return claims;
}

void
timeline_play(entity_t root) {
    scene_t* scene = timeline_scene(root);
    if (scene != NULL) {
        scene->timeline->clock.paused = false;
    }
}

void
timeline_pause(entity_t root) {
    scene_t* scene = timeline_scene(root);
    if (scene != NULL) {
        scene->timeline->clock.paused = true;
    }
}

bool
timeline_playing(entity_t root) {
    const scene_t* scene = timeline_scene(root);
    return scene != NULL && !scene->timeline->clock.paused;
}

void
timeline_seek(entity_t root, uint32_t t_ms) {
    scene_t* scene = timeline_scene(root);
    if (scene == NULL) {
        return;
    }
    scene_timeline_t* timeline = scene->timeline;
    scene_playhead_seek(&timeline->clock, t_ms);
    anim_playhead_advance(&timeline->clock, &timeline->tracks.clip, 0);
    timeline->dirty = true;
    scene_timeline_step(scene, 0);
    scene_skins_settle(scene);
    scene_animation_repose(scene, 0);
    timeline_camera_cut(scene);
}

uint32_t
timeline_time_ms(entity_t root) {
    const scene_t* scene = timeline_scene(root);
    return scene == NULL ? 0 : scene->timeline->clock.t_ms;
}

uint32_t
timeline_duration_ms(entity_t root) {
    const scene_t* scene = timeline_scene(root);
    return scene == NULL ? 0 : scene->timeline->tracks.clip.duration_ms;
}

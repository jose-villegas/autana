/* timeline: binds and samples the clip on a scene root's millisecond clock. */
#include "scene/timeline/timeline.h"

#include <math.h>
#include <string.h>

#include "math/scalar/units.h"
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
    for (int i = 0; i < timeline->field_count; i++) {
        scene_draw_camera_cut(scene, (scene_row_t)(timeline->fields[i].dirty - scene->flags));
    }
    for (int i = 0; i < scene->asset.constraint_count; i++) {
        scene_draw_camera_cut(scene, scene->constraints[i].values.entity);
    }
}

static void
timeline_apply_blocks(scene_t* scene) {
    const scene_timeline_t* timeline = scene->timeline;
    for (int i = 0; i < timeline->driver_count; i++) {
        const scene_timeline_driver_t* driver = &timeline->drivers[i];
        scene_animator_t* a = &scene->animators[driver->entity];
        const anim_block_sample_t sample =
            anim_blocks_sample(&driver->blocks, a->layer.clips, a->wrap, timeline->clock.t_ms);
        anim_layer_set(&a->layer, &sample);
        a->timeline_pose_pending = true;
    }
}

static void
timeline_apply_enabled(scene_t* scene) {
    const scene_timeline_t* timeline = scene->timeline;
    for (int i = 0; i < timeline->enabled_count; i++) {
        const scene_timeline_enabled_t* track = &timeline->enabled[i];
        const bool enabled = anim_block_curve_step(&track->curve, timeline->clock.t_ms, track->authored) != 0.0F;
        if (enabled != ((scene->flags[track->row] & SCENE_FLAG_ENABLED) != 0)) {
            scene_row_set_enabled(scene->root, track->row, enabled);
        }
    }
}

bool
scene_timeline_step(scene_t* scene, uint32_t dt_ms) {
    scene_timeline_t* timeline = scene->timeline;
    if (timeline == NULL) {
        return false;
    }
    anim_playhead_t* clock = &timeline->clock;
    const bool wrapped = anim_playhead_advance(clock, &timeline->tracks.clip, dt_ms);
    const uint32_t now = clock->t_ms;
    const bool applied = timeline->dirty || now != timeline->applied_ms || wrapped;
    if (applied) {
        scene_animation_apply_fields(scene, timeline->fields, timeline->field_count, clock, &timeline->tracks.clip);
        timeline_apply_blocks(scene);
        timeline_apply_enabled(scene);
        timeline->dirty = false;
        timeline->applied_ms = now;
    }
    if (wrapped) {
        timeline_camera_cut(scene);
    }
    return applied;
}

void
scene_timeline_sample_local(const scene_t* scene, scene_row_t row, const anim_playhead_t* time, transformf_t* local) {
    const scene_timeline_t* timeline = scene->timeline;
    const float seconds = anim_playhead_sample_seconds(time, &timeline->tracks.clip);
    const scene_timeline_fields_t range = timeline->row_fields[row];
    const int joint = (scene->flags[row] & SCENE_PROTECTED_TRANSFORM) ? scene_hinge_index(&scene->asset, row) : -1;
    const scene_hinge_t* hinge = joint >= 0 ? &scene->hinges[joint] : NULL;
    float angle = hinge == NULL ? 0.0F : scene->asset.hinges[joint].angle;
    for (int i = range.first; i < range.first + range.count; i++) {
        const anim_bound_t* sample = &timeline->fields[i];
        float value[ANIM_WIDTH_MAX];
        anim_track_sample(&sample->curve, seconds, value);
        if (hinge != NULL && sample->target == &hinge->angle) {
            angle = value[0];
        } else {
            const ptrdiff_t offset = (const uint8_t*)sample->target - (const uint8_t*)&scene->locals[row];
            memcpy((uint8_t*)local + offset, value, sample->curve.width * sizeof *value);
            local->cached = false;
        }
    }
    if (hinge != NULL) {
        scene_hinge_apply_pose(hinge, angle, local);
    }
}

transformf_t
scene_timeline_world_at(const scene_t* scene, scene_row_t row, uint32_t ms) {
    if (scene->timeline == NULL) {
        return scene_row_world(scene->root, row);
    }
    anim_playhead_t time = scene->timeline->clock;
    time.t_ms = ms;
    return scene_row_sample_world(scene->root, row, &time);
}

static scene_failure_t
resolve_block_slots(const scene_asset_t* asset, scene_row_t entity, const anim_binding_t* binding, uint8_t* slots) {
    const scene_asset_animator_t* animator = NULL;
    for (int i = 0; i < asset->animator_count; i++) {
        if (asset->animators[i].entity == entity) {
            animator = &asset->animators[i];
            break;
        }
    }
    const anim_track_t* curve = &binding->curve;
    const int count = animator == NULL ? 0 : animator->clip_count;
    const char* entity_name = asset->names[entity].name;
    for (int key = 0; key < curve->count; key++) {
        name_hash_t hash;
        (void)anim_name_decode(curve->values + key * ANIM_NAME_WIDTH, &hash);
        slots[key] = ANIM_LAYER_CLIP_NONE;
        for (int c = 0; c < count; c++) {
            const char* name = asset->clips[animator->first_clip + c];
            if (name_hash(name) == hash) {
                if (slots[key] == ANIM_LAYER_CLIP_NONE) {
                    slots[key] = (uint8_t)c;
                } else if (strcmp(name, asset->clips[animator->first_clip + slots[key]]) != 0) {
                    slots[key] = ANIM_LAYER_CLIP_NONE;
                    break;
                }
            }
        }
        if (slots[key] == ANIM_LAYER_CLIP_NONE) {
            return scene_binding_name_failure(binding, hash, "");
        }
    }
    return scene_failure_make(SCENE_OK, ASSET_OK, entity_name);
}

static bool
block_controls_fit(const anim_track_t* curve, int field) {
    uint32_t rate;
    for (int key = 0; key < curve->count; key++) {
        const float value = curve->values[key];
        if (field == ANIMATOR_FIELD_SPEED ? !anim_playhead_speed_q16(value, &rate)
                                          : !(value >= 0.0F && value <= (float)ANIMATOR_BLEND_MS_MAX)) {
            return false;
        }
    }
    return true;
}

static float
block_time_ms(float seconds) {
    return roundf(seconds * (float)MATH_MS_PER_S);
}

static bool
block_blends_fit(const anim_track_t* blocks, const anim_track_t* blends, const uint32_t* blend_times, uint32_t end) {
    int blend = -1;
    for (int key = 0; key < blocks->count; key++) {
        const uint32_t start = (uint32_t)block_time_ms(blocks->times[key]);
        while (blend + 1 < blends->count && blend_times[blend + 1] <= start) {
            blend++;
        }
        const uint32_t length = blend < 0 ? 0 : (uint32_t)blends->values[blend];
        if ((key == 0 && length != 0) || length > end - start) {
            return false;
        }
    }
    return true;
}

typedef enum {
    TIMELINE_FIELD,
    TIMELINE_TIME_KEYS,
    TIMELINE_TARGET_KEYS,
    TIMELINE_CLIP_KEYS,
} timeline_storage_t;

static timeline_storage_t
timeline_storage(anim_binding_location_t location) {
    if (location.component == SCENE_COMPONENT_ANIMATOR) {
        return location.field == ANIMATOR_FIELD_CLIP ? TIMELINE_CLIP_KEYS : TIMELINE_TIME_KEYS;
    }
    if (location.component == SCENE_COMPONENT_CNST && location.field < CONSTRAINT_STEP_COUNT) {
        return location.field < CONSTRAINT_TARGET_COUNT ? TIMELINE_TARGET_KEYS : TIMELINE_TIME_KEYS;
    }
    return location.component == SCENE_COMPONENT_ENTY ? TIMELINE_TIME_KEYS : TIMELINE_FIELD;
}

static scene_failure_t
validate_block_blends(const anim_tracks_t* timeline, const scene_timeline_blocks_t* blocks, const char* clip) {
    const uint32_t* times_ms = blocks->times_ms;
    for (int i = 0; i < timeline->count; i++) {
        anim_binding_t binding;
        (void)anim_tracks_binding_at(timeline, i, &binding);
        const anim_binding_location_t location = blocks->locations[i];
        if (timeline_storage(location) == TIMELINE_FIELD) {
            continue;
        }
        if (location.component == SCENE_COMPONENT_ANIMATOR && location.field == ANIMATOR_FIELD_BLEND_MS) {
            anim_track_t starts;
            (void)anim_tracks_find(timeline, binding.path_hash, binding.component, name_hash("clip"), &starts);
            if (!block_blends_fit(&starts, &binding.curve, times_ms, timeline->clip.duration_ms)) {
                return scene_binding_failure(ASSET_ERR_FORMAT, &binding);
            }
        }
        times_ms += binding.curve.count;
    }
    return scene_failure_make(SCENE_OK, ASSET_OK, clip);
}

scene_failure_t
scene_timeline_resolve(const scene_asset_t* asset, const scene_claims_t* claims, const anim_tracks_t* timeline,
                       scene_timeline_blocks_t* blocks, uint8_t* transforms) {
    for (int i = 0; i < timeline->count; i++) {
        anim_binding_t binding;
        (void)anim_tracks_binding_at(timeline, i, &binding);
        const timeline_storage_t storage = timeline_storage(blocks->locations[i]);
        if (storage != TIMELINE_FIELD) {
            blocks->time_keys += binding.curve.count;
        }
        if (blocks->locations[i].component == SCENE_COMPONENT_ENTY) {
            blocks->enabled_tracks++;
        }
        if (storage == TIMELINE_TARGET_KEYS) {
            blocks->target_keys += binding.curve.count;
        } else if (storage == TIMELINE_CLIP_KEYS) {
            blocks->drivers++;
            blocks->clip_keys += binding.curve.count;
        }
    }
    const char* clip = asset->timeline->clip;
    if (blocks->target_keys != 0) {
        if (blocks->target_keys > SIZE_MAX / sizeof *blocks->targets) {
            return scene_failure_make(SCENE_ERR_MEMORY, ASSET_OK, clip);
        }
        blocks->targets = memory_alloc(blocks->target_keys * sizeof *blocks->targets, MEMORY_APP);
        if (blocks->targets == NULL) {
            return scene_failure_make(SCENE_ERR_MEMORY, ASSET_OK, clip);
        }
    }
    if (blocks->time_keys > (SIZE_MAX - blocks->clip_keys) / sizeof *blocks->times_ms) {
        return scene_failure_make(SCENE_ERR_MEMORY, ASSET_OK, clip);
    }
    if (blocks->time_keys != 0) {
        blocks->times_ms = memory_alloc(blocks->time_keys * sizeof *blocks->times_ms + blocks->clip_keys, MEMORY_APP);
        if (blocks->times_ms == NULL) {
            return scene_failure_make(SCENE_ERR_MEMORY, ASSET_OK, clip);
        }
        blocks->slots = (uint8_t*)(blocks->times_ms + blocks->time_keys);
    }
    uint32_t* times_ms = blocks->times_ms;
    uint8_t* slots = blocks->slots;
    scene_row_t* target_keys = blocks->targets;
    const uint32_t end = timeline->clip.duration_ms;
    for (int i = 0; i < timeline->count; i++) {
        anim_binding_t binding;
        (void)anim_tracks_binding_at(timeline, i, &binding);
        const anim_binding_location_t location = blocks->locations[i];
        const timeline_storage_t storage = timeline_storage(location);
        if (storage == TIMELINE_FIELD) {
            continue;
        }
        const bool animator = location.component == SCENE_COMPONENT_ANIMATOR;
        const scene_row_t entity = (scene_row_t)location.target;
        if (storage == TIMELINE_TARGET_KEYS) {
            for (int key = 0; key < binding.curve.count; key++) {
                name_hash_t hash;
                (void)anim_name_decode(binding.curve.values + key * ANIM_NAME_WIDTH, &hash);
                const scene_failure_t result =
                    scene_constraint_target(asset, transforms, &binding, entity, hash, target_keys++);
                if (result.status != SCENE_OK) {
                    return result;
                }
            }
        } else if (storage == TIMELINE_CLIP_KEYS) {
            const scene_failure_t result = resolve_block_slots(asset, entity, &binding, slots);
            if (result.status != SCENE_OK) {
                return result;
            }
            slots += binding.curve.count;
        } else if (animator
                   && (!(claims->writes[entity] & scene_field_bit(location.component, ANIMATOR_FIELD_CLIP))
                       || !block_controls_fit(&binding.curve, location.field))) {
            return scene_binding_failure(ASSET_ERR_FORMAT, &binding);
        }
        uint32_t previous = 0;
        for (int key = 0; key < binding.curve.count; key++) {
            const float seconds = binding.curve.times[key];
            const float rounded = block_time_ms(seconds);
            if (seconds < 0.0F || rounded >= (float)ANIM_TRACKS_DURATION_MAX) {
                return scene_binding_failure(ASSET_ERR_FORMAT, &binding);
            }
            const uint32_t ms = (uint32_t)rounded;
            if (ms > end || (storage == TIMELINE_CLIP_KEYS && ms == end)
                || ((!animator || location.field != ANIMATOR_FIELD_BLEND_MS) && key > 0 && ms == previous)) {
                return scene_binding_failure(ASSET_ERR_FORMAT, &binding);
            }
            *times_ms++ = ms;
            previous = ms;
        }
    }
    return validate_block_blends(timeline, blocks, clip);
}

static void
layout_row_fields(scene_t* scene, const anim_binding_location_t* locations) {
    scene_timeline_t* timeline = scene->timeline;
    for (int i = 0; i < timeline->tracks.count; i++) {
        const anim_binding_location_t location = locations[i];
        if (location.component == SCENE_COMPONENT_TRANSFORM || location.component == SCENE_COMPONENT_HNGE) {
            timeline->row_fields[location.target].count++;
        }
    }
    for (int row = 0; row < scene->asset.entity_count; row++) {
        scene_timeline_fields_t* range = &timeline->row_fields[row];
        range->first = (uint16_t)timeline->field_count;
        timeline->field_count += range->count;
        range->count = 0;
    }
}

static bool
timeline_bind_tracks(scene_t* scene, const scene_anim_targets_t* targets, const anim_binding_location_t* locations) {
    scene_timeline_t* timeline = scene->timeline;
    const uint8_t* slots = timeline->slots;
    const uint32_t* times_ms = timeline->times_ms;
    const scene_row_t* target_keys = timeline->target_keys;
    layout_row_fields(scene, locations);
    for (int i = 0; i < timeline->tracks.count; i++) {
        anim_binding_t binding;
        (void)anim_tracks_binding_at(&timeline->tracks, i, &binding);
        const anim_binding_location_t location = locations[i];
        const timeline_storage_t storage = timeline_storage(location);
        const anim_target_t* target = &targets->targets[location.target];
        if (location.component == SCENE_COMPONENT_ANIMATOR) {
            scene_animator_t* a = &scene->animators[location.target];
            if (a->timeline_driver == 0) {
                a->timeline_driver = (uint16_t)++timeline->driver_count;
            }
            scene_timeline_driver_t* driver = &timeline->drivers[a->timeline_driver - 1];
            driver->entity = (scene_row_t)location.target;
            if (storage == TIMELINE_CLIP_KEYS) {
                driver->blocks.starts_ms = times_ms;
                driver->blocks.slots = slots;
                driver->blocks.count = binding.curve.count;
                slots += binding.curve.count;
            } else {
                anim_block_curve_t* curve =
                    location.field == ANIMATOR_FIELD_SPEED ? &driver->blocks.speed : &driver->blocks.blend;
                *curve = (anim_block_curve_t){times_ms, binding.curve.values, binding.curve.count};
            }
            times_ms += binding.curve.count;
        } else if (location.component == SCENE_COMPONENT_ENTY) {
            timeline->enabled[timeline->enabled_count++] =
                (scene_timeline_enabled_t){.curve = {times_ms, binding.curve.values, binding.curve.count},
                                           .row = location.target,
                                           .authored = (scene->flags[location.target] & SCENE_FLAG_ENABLED) != 0};
            times_ms += binding.curve.count;
        } else if (storage != TIMELINE_FIELD) {
            scene_constraint_t* constraint = scene_constraint_find(scene, (scene_row_t)location.target);
            scene_constraint_curve_t* curve = &constraint->curves[location.field];
            *curve = (scene_constraint_curve_t){
                .times = times_ms, .values = binding.curve.values, .count = binding.curve.count};
            if (storage == TIMELINE_TARGET_KEYS) {
                curve->targets = target_keys;
                target_keys += binding.curve.count;
            }
            times_ms += binding.curve.count;
        } else {
            int index;
            if (location.component == SCENE_COMPONENT_TRANSFORM || location.component == SCENE_COMPONENT_HNGE) {
                scene_timeline_fields_t* range = &timeline->row_fields[location.target];
                index = range->first + range->count++;
            } else {
                index = timeline->field_count++;
            }
            if (anim_bind_field(&binding, &target->components[location.component], 1, target->dirty, target->dirty_bit,
                                &timeline->fields[index])
                != ANIM_BIND_OK) {
                return false;
            }
        }
    }
    return true;
}

scene_failure_t
scene_timeline_bind(scene_t* scene, const anim_binding_location_t* locations) {
    scene_timeline_t* timeline = scene->timeline;
    const char* clip = scene->asset.timeline->clip;
    if (timeline->tracks.count != 0) {
        const scene_anim_targets_t targets = scene_anim_targets(&scene->asset, scene);
        if (targets.block == NULL) {
            return scene_failure_make(SCENE_ERR_MEMORY, ASSET_OK, clip);
        }
        const bool bound = timeline_bind_tracks(scene, &targets, locations);
        memory_free(targets.block);
        if (!bound) {
            return scene_failure_make(SCENE_ERR_ASSET, ASSET_ERR_FORMAT, clip);
        }
    }
    timeline->dirty = true;
    scene_timeline_step(scene, 0);
    return scene_failure_make(SCENE_OK, ASSET_OK, clip);
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
    anim_playhead_seek(&timeline->clock, &timeline->tracks.clip, t_ms);
    timeline->dirty = true;
    scene_drivers_step(scene, scene_timeline_step(scene, 0));
    for (int i = 0; i < timeline->driver_count; i++) {
        scene->animators[timeline->drivers[i].entity].dirty = true;
    }
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

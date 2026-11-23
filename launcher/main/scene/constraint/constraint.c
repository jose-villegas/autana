/* Constraint target poses are sampled on the scene clock. */
#include "scene/constraint/constraint.h"

#include <float.h>
#include <math.h>

#include "kinematics/rig/rig.h"
#include "math/scalar/ease.h"
#include "scene/scene_internal.h"

static const anim_field_t FIELDS[] = {
    ANIM_FIELD_STEP(scene_asset_constraint_t, follow, ANIM_VALUE_NAME),
    ANIM_FIELD_STEP(scene_asset_constraint_t, look_at, ANIM_VALUE_NAME),
    ANIM_FIELD_STEP(scene_asset_constraint_t, lag_ms, ANIM_VALUE_FLOAT),
    ANIM_FIELD_STEP(scene_asset_constraint_t, handoff_ms, ANIM_VALUE_FLOAT),
    ANIM_FIELD(scene_asset_constraint_t, offset, ANIM_VALUE_VEC3),
    ANIM_FIELD(scene_asset_constraint_t, look_offset, ANIM_VALUE_VEC3),
    ANIM_FIELD(scene_asset_constraint_t, weight, ANIM_VALUE_FLOAT),
};

const anim_component_fields_t CONSTRAINT_FIELDS = {ANIM_COMPONENT_CONSTRAINT, FIELDS, CONSTRAINT_FIELD_COUNT};

_Static_assert(SCENE_ASSET_BIND_WORLD == RIG_BIND_WORLD && SCENE_ASSET_BIND_TARGET == RIG_BIND_TARGET
                   && SCENE_ASSET_BIND_TARGET_YAW == RIG_BIND_TARGET_YAW,
               "SCNE and rig use the same bind modes");

scene_failure_t
scene_constraint_target(const scene_asset_t* asset, uint8_t* transforms, const anim_binding_t* binding,
                        name_hash_t hash, scene_row_t* out) {
    *out = scene_asset_find_row(asset, 0, hash);
    scene_row_t driver = SCENE_ROW_NONE;
    for (scene_row_t row = *out; row != SCENE_ROW_NONE; row = scene_asset_parent(asset, row)) {
        if (transforms[row] & (SCENE_PROPERTY_TRANSFORM | SCENE_CONSTRAINT_TRANSFORM)) {
            driver = row;
            break;
        }
        transforms[row] |= SCENE_PROTECTED_TRANSFORM;
    }
    if (driver == SCENE_ROW_NONE && (*out != SCENE_ROW_NONE || hash == NAME_HASH_NONE)) {
        return scene_failure_make(SCENE_OK, ASSET_OK, binding->path);
    }
    const char* detail = driver != SCENE_ROW_NONE ? asset->names[driver].name
                         : binding->curve.count   ? " (track)"
                                                  : " (row)";
    return scene_binding_name_failure(binding, hash, detail);
}

scene_failure_t
scene_constraint_resolve(const scene_asset_t* asset, uint8_t* transforms, scene_row_t* targets) {
    for (int i = 0; i < asset->constraint_count; i++) {
        const scene_asset_constraint_t* c = &asset->constraints[i];
        const scene_row_t owner = c->entity;
        anim_binding_t source = {.path = asset->names[owner].name, .component = ANIM_COMPONENT_CONSTRAINT};
        for (scene_row_t p = scene_asset_parent(asset, owner); p != SCENE_ROW_NONE; p = scene_asset_parent(asset, p)) {
            if (transforms[p] & (SCENE_PROPERTY_TRANSFORM | SCENE_CONSTRAINT_TRANSFORM)) {
                return scene_failure_make(SCENE_ERR_ASSET, ASSET_ERR_FORMAT, asset->names[p].name);
            }
            transforms[p] |= SCENE_PROTECTED_TRANSFORM;
        }
        const name_hash_t names[CONSTRAINT_TARGET_COUNT] = {c->follow, c->look_at};
        for (int f = 0; f < CONSTRAINT_TARGET_COUNT; f++) {
            source.field = FIELDS[f].name;
            const scene_failure_t result = scene_constraint_target(asset, transforms, &source, names[f], targets++);
            if (result.status != SCENE_OK) {
                return result;
            }
        }
    }
    return scene_failure_make(SCENE_OK, ASSET_OK, "");
}

scene_constraint_t*
scene_constraint_find(const scene_t* scene, scene_row_t row) {
    for (int i = 0; i < scene->asset.constraint_count; i++) {
        if (scene->asset.constraints[i].entity == row) {
            return &scene->constraints[i];
        }
    }
    return NULL;
}

static bool
path_contains(const scene_asset_t* asset, scene_row_t target, scene_row_t row) {
    for (; target != SCENE_ROW_NONE; target = scene_asset_parent(asset, target)) {
        if (target == row) {
            return true;
        }
    }
    return false;
}

bool
scene_constraint_protects(const scene_t* scene, scene_row_t row) {
    const scene_asset_t* asset = &scene->asset;
    for (int i = 0; i < asset->constraint_count; i++) {
        const scene_constraint_t* c = &scene->constraints[i];
        if (path_contains(asset, c->values.entity, row)) {
            return true;
        }
        for (int f = 0; f < CONSTRAINT_TARGET_COUNT; f++) {
            if (path_contains(asset, c->targets[f], row)) {
                return true;
            }
            for (int key = 0; key < c->curves[f].count; key++) {
                if (path_contains(asset, c->curves[f].targets[key], row)) {
                    return true;
                }
            }
        }
    }
    return false;
}

void
scene_constraint_open(scene_t* scene) {
    for (int i = 0; i < scene->asset.constraint_count; i++) {
        scene_constraint_t* c = &scene->constraints[i];
        c->own = scene->locals[c->values.entity];
    }
}

static uint32_t
control_ms(const scene_constraint_curve_t* curve, float fallback, uint32_t now) {
    const int key = anim_track_key_at_ms(curve->times, curve->count, now);
    const float ms = roundf(key < 0 ? fallback : curve->values[key]);
    return ms >= (float)UINT32_MAX ? UINT32_MAX : (uint32_t)ms;
}

static float
handoff_weight(const scene_constraint_t* c, const scene_constraint_curve_t* curve, int key, uint32_t now) {
    const uint32_t start = curve->times[key];
    const uint32_t duration = control_ms(&c->curves[CONSTRAINT_FIELD_HANDOFF_MS], c->values.handoff_ms, start);
    return ease_smooth(now - start, duration);
}

static bool
target_changes(const scene_constraint_curve_t* curve, int key, scene_row_t initial) {
    return curve->targets[key] != (key == 0 ? initial : curve->targets[key - 1]);
}

static vec3f_t
follow_position(const scene_t* scene, const scene_asset_constraint_t* c, scene_row_t row, uint32_t time, vec3f_t own) {
    if (row == SCENE_ROW_NONE) {
        return own;
    }
    const transformf_t target = scene_timeline_world_at(scene, row, time);
    return rig_follow_position(&target, c->offset, (rig_bind_t)c->bind);
}

static quatf_t
look_rotation(const scene_t* scene, const scene_asset_constraint_t* c, scene_row_t row, uint32_t time, vec3f_t from,
              quatf_t own) {
    if (row == SCENE_ROW_NONE) {
        return own;
    }
    const transformf_t target = scene_timeline_world_at(scene, row, time);
    const mat4f_t frame = transformf_compute_matrix(&target);
    return rig_look_rotation(from, mat4f_apply(&frame, c->look_offset), vec3f_up());
}

typedef struct {
    vec3f_t position;
    quatf_t rotation;
} scene_constraint_pose_t;

static bool
sample_handoff(const scene_t* scene, const scene_constraint_t* c, int field, uint32_t now, uint32_t sample,
               const transformf_t* own, scene_constraint_pose_t* result) {
    const scene_constraint_curve_t* curve = &c->curves[field];
    const scene_row_t initial = c->targets[field];
    const int last = anim_track_key_at_ms(curve->times, curve->count, now);
    int first = last;
    while (first >= 0 && (!target_changes(curve, first, initial) || handoff_weight(c, curve, first, now) < 1.0F)) {
        first--;
    }
    bool active = false;
    for (int key = first; key <= last; key++) {
        if (key > first && !target_changes(curve, key, initial)) {
            continue;
        }
        const scene_row_t target = key < 0 ? initial : curve->targets[key];
        const float weight = key == first ? 1.0F : handoff_weight(c, curve, key, now);
        if (weight == 0.0F) {
            continue;
        }
        active |= target != SCENE_ROW_NONE;
        if (field == CONSTRAINT_FIELD_FOLLOW) {
            const vec3f_t to = follow_position(scene, &c->values, target, sample, own->position);
            result->position = weight == 1.0F ? to : vec3f_lerp(result->position, to, weight);
        } else {
            const quatf_t to = look_rotation(scene, &c->values, target, sample, result->position, own->rotation);
            result->rotation = weight == 1.0F ? to : quatf_nlerp(result->rotation, to, weight);
        }
    }
    return active;
}

static uint32_t
follow_time(const scene_t* scene, uint32_t now, uint32_t lag) {
    if (now >= lag) {
        return now - lag;
    }
    if (scene->timeline != NULL && scene->timeline->clock.wrap == ANIM_LOOP) {
        const uint32_t end = scene->timeline->tracks.clip.duration_ms;
        if (end != 0) {
            return (end - (lag - now) % end) % end;
        }
    }
    return 0;
}

static bool
path_placed(const scene_t* scene, scene_row_t row) {
    for (; row != SCENE_ROW_NONE; row = scene_row_parent(scene, row)) {
        if (scene->flags[row] & SCENE_FLAG_CONSTRAINT_INPUT) {
            return true;
        }
    }
    return false;
}

static bool
constraint_inputs_placed(const scene_t* scene, const scene_constraint_t* c) {
    if (path_placed(scene, scene_row_parent(scene, c->values.entity))) {
        return true;
    }
    for (int f = 0; f < CONSTRAINT_TARGET_COUNT; f++) {
        if (path_placed(scene, c->targets[f])) {
            return true;
        }
        for (int key = 0; key < c->curves[f].count; key++) {
            if (path_placed(scene, c->curves[f].targets[key])) {
                return true;
            }
        }
    }
    return false;
}

/* Keep the blend's transform temporaries off the target sampling call chain. */
static __attribute__((noinline)) void
blend_local(transformf_t* local, const transformf_t* frame, const transformf_t* own,
            const scene_constraint_pose_t* pose, bool follows, bool looks, float weight) {
    const transformf_t world = {.position = pose->position, .rotation = pose->rotation, .scale = own->scale};
    const transformf_t blended = transformf_blend(own, &world, weight);
    const transformf_t relative = transformf_relative(frame, &blended);
    if (follows) {
        local->position = relative.position;
    }
    if (looks) {
        local->rotation = relative.rotation;
    }
    local->cached = false;
}

static void
apply_constraint(scene_t* scene, const scene_constraint_t* c, const anim_playhead_t* time) {
    const scene_row_t row = c->values.entity;
    transformf_t local = time == NULL ? c->own : scene_timeline_local_at(scene, row, time, c->own);
    if (c->values.weight > 0.0F) {
        const scene_row_t parent = scene_row_parent(scene, row);
        const transformf_t frame = scene_row_world(scene->root, parent == SCENE_ROW_NONE ? SCENE_ROW_ROOT : parent);
        const vec3f_t scale = frame.scale;
        if (fabsf(scale.x) >= FLT_MIN && fabsf(scale.y) >= FLT_MIN && fabsf(scale.z) >= FLT_MIN) {
            const transformf_t own = transformf_compose(&frame, &local);
            const uint32_t now = time == NULL ? 0 : time->t_ms;
            const uint32_t lag = control_ms(&c->curves[CONSTRAINT_FIELD_LAG_MS], c->values.lag_ms, now);
            scene_constraint_pose_t pose = {own.position, own.rotation};
            const bool follows =
                sample_handoff(scene, c, CONSTRAINT_FIELD_FOLLOW, now, follow_time(scene, now, lag), &own, &pose);
            const bool looks = sample_handoff(scene, c, CONSTRAINT_FIELD_LOOK_AT, now, now, &own, &pose);
            if (follows || looks) {
                blend_local(&local, &frame, &own, &pose, follows, looks, c->values.weight);
            }
        }
    }
    scene_row_set_local(scene->root, row, local);
}

void
scene_constraint_step(scene_t* scene, bool applied) {
    const anim_playhead_t* time = scene->timeline == NULL ? NULL : &scene->timeline->clock;
    const bool placed = scene->constraint_inputs_placed;
    if (!applied && !placed) {
        return;
    }
    for (int i = 0; i < scene->asset.constraint_count; i++) {
        const scene_constraint_t* c = &scene->constraints[i];
        if (applied || constraint_inputs_placed(scene, c)) {
            apply_constraint(scene, c, time);
            scene->flags[c->values.entity] &= (uint8_t)~SCENE_FLAG_CONSTRAINT_INPUT;
        }
    }
    if (placed) {
        for (int row = 0; row < scene->asset.entity_count; row++) {
            scene->flags[row] &= (uint8_t)~SCENE_FLAG_CONSTRAINT_INPUT;
        }
    }
    scene->constraint_inputs_placed = false;
}

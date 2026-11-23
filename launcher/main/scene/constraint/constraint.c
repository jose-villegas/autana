/* Constraint target poses are sampled on the scene clock. */
#include "scene/constraint/constraint.h"

#include <float.h>
#include <math.h>

#include "kinematics/rig/rig.h"
#include "math/scalar/ease.h"
#include "scene/scene_internal.h"

static const anim_field_t FIELDS[] = {
    ANIM_FIELD_STEP_OPTIONAL_NAME(scene_asset_constraint_t, follow, ANIM_VALUE_NAME),
    ANIM_FIELD_STEP_OPTIONAL_NAME(scene_asset_constraint_t, look_at, ANIM_VALUE_NAME),
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

static scene_row_t
scene_constraint_protects(const scene_asset_t* asset, uint8_t* transforms, scene_row_t row, scene_row_t owner) {
    if (scene_asset_row_within(asset, row, owner)) {
        return owner;
    }
    for (; row != SCENE_ROW_NONE; row = scene_asset_parent(asset, row)) {
        if (transforms[row] & (SCENE_PROPERTY_TRANSFORM | SCENE_CONSTRAINT_TRANSFORM | SCENE_HINGE_TRANSFORM)) {
            return row;
        }
        transforms[row] |= SCENE_PROTECTED_TRANSFORM;
    }
    return SCENE_ROW_NONE;
}

scene_failure_t
scene_constraint_target(const scene_asset_t* asset, uint8_t* transforms, const anim_binding_t* binding,
                        scene_row_t owner, name_hash_t hash, scene_row_t* out) {
    *out = scene_asset_find_row(asset, 0, hash);
    const scene_row_t driver = scene_constraint_protects(asset, transforms, *out, owner);
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
        if (transforms[owner] & SCENE_HINGE_TRANSFORM) {
            const anim_binding_t hinge = {
                .path = source.path, .component = ANIM_COMPONENT_HINGE, .field = HINGE_FIELDS.fields[0].name};
            return scene_binding_failure(ASSET_ERR_FORMAT, &hinge);
        }
        const scene_row_t driver =
            scene_constraint_protects(asset, transforms, scene_asset_parent(asset, owner), owner);
        if (driver != SCENE_ROW_NONE) {
            return scene_failure_make(SCENE_ERR_ASSET, ASSET_ERR_FORMAT, asset->names[driver].name);
        }
        const name_hash_t names[CONSTRAINT_TARGET_COUNT] = {c->follow, c->look_at};
        for (int f = 0; f < CONSTRAINT_TARGET_COUNT; f++) {
            source.field = FIELDS[f].name;
            const scene_failure_t result =
                scene_constraint_target(asset, transforms, &source, owner, names[f], targets++);
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
    return (uint32_t)roundf(key < 0 ? fallback : curve->values[key]);
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
look_rotation(const scene_t* scene, const scene_asset_constraint_t* c, scene_row_t row, vec3f_t from, quatf_t own) {
    if (row == SCENE_ROW_NONE) {
        return own;
    }
    const anim_playhead_t* time = scene->timeline == NULL ? NULL : &scene->timeline->clock;
    const vec3f_t target = scene_row_sample_point(scene->root, row, time, c->look_offset);
    return rig_look_rotation(from, target, vec3f_up());
}

typedef struct {
    vec3f_t position;
    quatf_t rotation;
} scene_constraint_pose_t;

static int
handoff_first(const scene_constraint_t* c, int field, int last, uint32_t now) {
    const scene_constraint_curve_t* curve = &c->curves[field];
    const scene_row_t initial = c->targets[field];
    int first = last;
    while (first >= 0 && (!target_changes(curve, first, initial) || handoff_weight(c, curve, first, now) < 1.0F)) {
        first--;
    }
    return first;
}

static float
handoff_key_weight(const scene_constraint_t* c, int field, int first, int key, uint32_t now) {
    if (key == first) {
        return 1.0F;
    }
    const scene_constraint_curve_t* curve = &c->curves[field];
    return target_changes(curve, key, c->targets[field]) ? handoff_weight(c, curve, key, now) : 0.0F;
}

static bool
sample_handoff(const scene_t* scene, const scene_constraint_t* c, int field, uint32_t now, uint32_t sample,
               const transformf_t* own, scene_constraint_pose_t* result) {
    const scene_constraint_curve_t* curve = &c->curves[field];
    const scene_row_t initial = c->targets[field];
    const int last = anim_track_key_at_ms(curve->times, curve->count, now);
    const int first = handoff_first(c, field, last, now);
    bool active = false;
    for (int key = first; key <= last; key++) {
        const scene_row_t target = key < 0 ? initial : curve->targets[key];
        const float weight = handoff_key_weight(c, field, first, key, now);
        if (weight == 0.0F) {
            continue;
        }
        active |= target != SCENE_ROW_NONE;
        if (field == CONSTRAINT_FIELD_FOLLOW) {
            const vec3f_t to = follow_position(scene, &c->values, target, sample, own->position);
            result->position = weight == 1.0F ? to : vec3f_lerp(result->position, to, weight);
        } else {
            const quatf_t to = look_rotation(scene, &c->values, target, result->position, own->rotation);
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
constraint_inputs_placed(const scene_t* scene, const scene_constraint_t* c, uint32_t now) {
    if (path_placed(scene, scene_row_parent(scene, c->values.entity))) {
        return true;
    }
    for (int f = 0; f < CONSTRAINT_TARGET_COUNT; f++) {
        const scene_constraint_curve_t* curve = &c->curves[f];
        const int last = anim_track_key_at_ms(curve->times, curve->count, now);
        const int first = handoff_first(c, f, last, now);
        for (int key = first; key <= last; key++) {
            const scene_row_t target = key < 0 ? c->targets[f] : curve->targets[key];
            if (handoff_key_weight(c, f, first, key, now) != 0.0F && path_placed(scene, target)) {
                return true;
            }
        }
    }
    return false;
}

static void
blend_local(transformf_t* local, const transformf_t* frame, const transformf_t* own,
            const scene_constraint_pose_t* pose, bool follows, bool looks, float weight) {
    if (follows) {
        local->position = transformf_inverse_point(frame, pose->position);
    }
    if (looks) {
        const quatf_t blended = quatf_nlerp(own->rotation, pose->rotation, weight);
        local->rotation = quatf_normalize(quatf_mul(quatf_conjugate(frame->rotation), blended));
    }
    local->cached = false;
}

static void
apply_constraint(scene_t* scene, const scene_constraint_t* c, const anim_playhead_t* time) {
    const scene_row_t row = c->values.entity;
    transformf_t local = time == NULL ? c->own : scene_timeline_local_at(scene, row, time, c->own);
    if (c->values.weight > 0.0F) {
        const float weight = fminf(c->values.weight, 1.0F);
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
            if (follows) {
                pose.position = vec3f_add(vec3f_scale(own.position, 1.0F - weight), vec3f_scale(pose.position, weight));
            }
            const bool looks = sample_handoff(scene, c, CONSTRAINT_FIELD_LOOK_AT, now, now, &own, &pose);
            if (follows || looks) {
                blend_local(&local, &frame, &own, &pose, follows, looks, weight);
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
        if (applied || constraint_inputs_placed(scene, c, time == NULL ? 0 : time->t_ms)) {
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

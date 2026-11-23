/* Constraint target poses are sampled on the scene clock. */
#include "scene/constraint/constraint.h"

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

static bool
driven(const scene_asset_t* asset, const scene_field_mask_t* writes, scene_row_t row) {
    if (writes[row] & SCENE_PROPERTY_TRANSFORM) {
        return true;
    }
    for (int i = 0; i < asset->constraint_count; i++) {
        if (asset->constraints[i].entity == row) {
            return true;
        }
    }
    for (int i = 0; i < asset->hinge_count; i++) {
        if (asset->hinges[i].entity == row) {
            return true;
        }
    }
    return false;
}

scene_failure_t
scene_constraint_target(const scene_asset_t* asset, scene_field_mask_t* writes, const anim_binding_t* binding,
                        name_hash_t hash, scene_row_t* out) {
    *out = scene_asset_find_row(asset, 0, hash);
    scene_row_t driver = SCENE_ROW_NONE;
    for (scene_row_t row = *out; row != SCENE_ROW_NONE; row = scene_asset_parent(asset, row)) {
        if (driven(asset, writes, row)) {
            driver = row;
            break;
        }
        writes[row] |= SCENE_PROTECTED_TRANSFORM;
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
scene_constraint_resolve(const scene_asset_t* asset, scene_field_mask_t* writes, scene_row_t* targets) {
    for (int i = 0; i < asset->constraint_count; i++) {
        const scene_asset_constraint_t* c = &asset->constraints[i];
        const scene_row_t owner = c->entity;
        anim_binding_t source = {.path = asset->names[owner].name, .component = ANIM_COMPONENT_CONSTRAINT};
        for (int h = 0; h < asset->hinge_count; h++) {
            if (asset->hinges[h].entity == owner) {
                return scene_failure_make(SCENE_ERR_ASSET, ASSET_ERR_FORMAT, source.path);
            }
        }
        for (scene_row_t p = scene_asset_parent(asset, owner); p != SCENE_ROW_NONE; p = scene_asset_parent(asset, p)) {
            if (driven(asset, writes, p)) {
                return scene_failure_make(SCENE_ERR_ASSET, ASSET_ERR_FORMAT, asset->names[p].name);
            }
            writes[p] |= SCENE_PROTECTED_TRANSFORM;
        }
        const name_hash_t names[CONSTRAINT_TARGET_COUNT] = {c->follow, c->look_at};
        for (int f = 0; f < CONSTRAINT_TARGET_COUNT; f++) {
            source.field = FIELDS[f].name;
            const scene_failure_t result = scene_constraint_target(asset, writes, &source, names[f], targets++);
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
    const float ms = roundf(key < 0 ? fallback : curve->values[key]);
    return ms >= (float)UINT32_MAX ? UINT32_MAX : (uint32_t)ms;
}

typedef struct {
    scene_row_t from, to;
    float weight;
} handoff_t;

static handoff_t
handoff(const scene_constraint_t* c, int field, uint32_t now, uint32_t duration) {
    const scene_constraint_curve_t* curve = &c->curves[field];
    const scene_row_t initial = c->targets[field];
    const int key = anim_track_key_at_ms(curve->times, curve->count, now);
    if (key < 0) {
        return (handoff_t){initial, initial, 1.0F};
    }
    const scene_row_t to = curve->targets[key];
    int start = key;
    while (start > 0 && curve->targets[start - 1] == to) {
        start--;
    }
    const scene_row_t from = start == 0 ? initial : curve->targets[start - 1];
    const float weight = ease_smooth(now - curve->times[start], duration);
    return weight == 0.0F ? (handoff_t){from, from, 1.0F} : (handoff_t){from, to, weight};
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
    return rig_look_rotation(from, mat4f_apply(&frame, c->look_offset), vec3f_unit_y());
}

static bool
handoff_active(handoff_t h) {
    return h.from != h.to && h.weight < 1.0F;
}

static transformf_t
constrain(const scene_t* scene, const scene_constraint_t* c, const transformf_t* own, uint32_t now, bool* follows,
          bool* looks) {
    const scene_asset_constraint_t* v = &c->values;
    const uint32_t lag = control_ms(&c->curves[CONSTRAINT_FIELD_LAG_MS], v->lag_ms, now);
    const uint32_t time = now > lag ? now - lag : 0;
    const uint32_t duration = control_ms(&c->curves[CONSTRAINT_FIELD_HANDOFF_MS], v->handoff_ms, now);
    const handoff_t follow = handoff(c, CONSTRAINT_FIELD_FOLLOW, now, duration);
    const handoff_t look = handoff(c, CONSTRAINT_FIELD_LOOK_AT, now, duration);
    transformf_t result = *own;
    const bool follow_blend = handoff_active(follow);
    const bool look_blend = handoff_active(look);
    *follows = follow.to != SCENE_ROW_NONE || follow_blend;
    *looks = look.to != SCENE_ROW_NONE || look_blend;
    result.position = follow_position(scene, v, follow.to, time, own->position);
    if (follow_blend) {
        const vec3f_t from = follow_position(scene, v, follow.from, time, own->position);
        result.position = vec3f_lerp(from, result.position, follow.weight);
    }
    result.rotation = look_rotation(scene, v, look.to, now, result.position, own->rotation);
    if (look_blend) {
        const quatf_t from = look_rotation(scene, v, look.from, now, result.position, own->rotation);
        result.rotation = quatf_nlerp(from, result.rotation, look.weight);
    }
    return transformf_blend(own, &result, v->weight);
}

static void
apply_constraint(scene_t* scene, const scene_constraint_t* c, const anim_playhead_t* time) {
    const scene_row_t row = c->values.entity;
    const transformf_t local = time == NULL ? c->own : scene_timeline_local_at(scene, row, time, c->own);
    const scene_row_t parent = scene_row_parent(scene, row);
    const transformf_t frame = scene_row_world(scene->root, parent == SCENE_ROW_NONE ? SCENE_ROW_ROOT : parent);
    const transformf_t own = transformf_compose(&frame, &local);
    bool follows, looks;
    if (c->values.weight > 0.0F) {
        const transformf_t world = constrain(scene, c, &own, time == NULL ? 0 : time->t_ms, &follows, &looks);
        if (follows || looks) {
            scene_row_set_world(scene->root, row, world);
            if (!follows) {
                scene->locals[row].position = local.position;
            }
            if (!looks) {
                scene->locals[row].rotation = local.rotation;
            }
            scene->locals[row].scale = local.scale;
            return;
        }
    }
    scene_row_set_local(scene->root, row, local);
}

void
scene_constraint_step(scene_t* scene) {
    const anim_playhead_t* time = scene->timeline == NULL ? NULL : &scene->timeline->clock;
    for (int i = 0; i < scene->asset.constraint_count; i++) {
        apply_constraint(scene, &scene->constraints[i], time);
    }
}

/* Constraints read the placed and timeline-sampled world poses. */
#include "scene/constraint/constraint.h"

#include <assert.h>
#include <float.h>
#include <string.h>

#include "kinematics/rig/rig.h"
#include "scene/scene_internal.h"

static const anim_field_t WEIGHT_FIELDS[] = {CONSTRAINT_WEIGHT_FIELD_LIST};
const anim_component_fields_t POSITION_CONSTRAINT_FIELDS = {ANIM_COMPONENT_POSITION_CONSTRAINT, WEIGHT_FIELDS,
                                                            CONSTRAINT_WEIGHT_FIELD_COUNT};
const anim_component_fields_t AIM_CONSTRAINT_FIELDS = {ANIM_COMPONENT_AIM_CONSTRAINT, WEIGHT_FIELDS,
                                                       CONSTRAINT_WEIGHT_FIELD_COUNT};

_Static_assert(sizeof WEIGHT_FIELDS / sizeof *WEIGHT_FIELDS == CONSTRAINT_WEIGHT_FIELD_COUNT
                   && offsetof(scene_constraint_weights_t, weight3)
                          == (CONSTRAINT_WEIGHT_FIELD_COUNT - 1) * sizeof(float),
               "constraint weight bindings match the sampled columns");
_Static_assert(SCENE_ASSET_BIND_WORLD_SPACE == RIG_BIND_WORLD && SCENE_ASSET_BIND_LOCK_TO_TARGET == RIG_BIND_TARGET
                   && SCENE_ASSET_BIND_LOCK_TO_TARGET_WITH_WORLD_UP == RIG_BIND_TARGET_YAW,
               "SCNE and rig use the same binding modes");

#define AIM_DISTANCE_SQUARED_MIN (FLT_EPSILON * FLT_EPSILON)

static scene_row_t
source_row(const scene_asset_t* asset, int index) {
    return scene_asset_find_row(asset, asset->sources[index].target);
}

static scene_row_t
protect_path(scene_claims_t* claims, scene_row_t row) {
    for (; row != SCENE_ROW_NONE; row = scene_asset_parent(claims->asset, row)) {
        if (claims->protected_rows[row] & (SCENE_CLAIM_DRIVEN | SCENE_HINGE_TRANSFORM)) {
            return row;
        }
        claims->protected_rows[row] |= SCENE_PROTECTED_TRANSFORM;
    }
    return SCENE_ROW_NONE;
}

static scene_row_t
protect_inputs(scene_claims_t* claims, scene_row_t owner, int first, int count, scene_row_t up) {
    scene_row_t driver = protect_path(claims, scene_asset_parent(claims->asset, owner));
    for (int i = 0; driver == SCENE_ROW_NONE && i < count; i++) {
        driver = protect_path(claims, source_row(claims->asset, first + i));
    }
    return driver == SCENE_ROW_NONE ? protect_path(claims, up) : driver;
}

static void
claim_transform(scene_claims_t* claims, scene_row_t row, int field) {
    claims->protected_rows[row] |= SCENE_CLAIM_DRIVEN | SCENE_PROTECTED_TRANSFORM;
    const bool claimed = scene_field_claim(claims, row, scene_field_bit(SCENE_COMPONENT_TRANSFORM, field));
    assert(claimed);
    (void)claimed;
}

static scene_failure_t
hinge_conflict(const scene_asset_t* asset, scene_row_t owner) {
    const anim_binding_t hinge = {
        .path = asset->names[owner].name, .component = ANIM_COMPONENT_HINGE, .field = HINGE_FIELDS.fields[0].name};
    return scene_binding_failure(ASSET_ERR_FORMAT, &hinge);
}

static scene_failure_t
driven_failure(const scene_claims_t* claims, scene_row_t row) {
    if (claims->protected_rows[row] & SCENE_HINGE_TRANSFORM) {
        return hinge_conflict(claims->asset, row);
    }
    const scene_field_mask_t position = scene_field_bit(SCENE_COMPONENT_TRANSFORM, ANIM_POSE_POSITION_FIELD);
    const int field = claims->writes[row] & position ? ANIM_POSE_POSITION_FIELD : ANIM_POSE_ROTATION_FIELD;
    const anim_binding_t binding = {.path = claims->asset->names[row].name,
                                    .component = ANIM_COMPONENT_TRANSFORM,
                                    .field = ANIM_POSE_TRANSFORM_FIELDS.fields[field].name};
    return scene_binding_failure(ASSET_ERR_FORMAT, &binding);
}

scene_failure_t
scene_constraint_claim(scene_claims_t* claims) {
    const scene_asset_t* asset = claims->asset;
    for (int i = 0; i < asset->hinge_count; i++) {
        const scene_row_t owner = asset->hinges[i].entity;
        claims->protected_rows[owner] |= SCENE_HINGE_TRANSFORM | SCENE_PROTECTED_TRANSFORM;
        (void)scene_field_claim(claims, owner,
                                scene_field_bit(SCENE_COMPONENT_TRANSFORM, ANIM_POSE_POSITION_FIELD)
                                    | scene_field_bit(SCENE_COMPONENT_TRANSFORM, ANIM_POSE_ROTATION_FIELD));
    }
    const int positions = asset->position_constraint_count;
    const int constraints = positions + asset->aim_constraint_count;
    for (int i = 0; i < constraints; i++) {
        const bool position = i < positions;
        const scene_row_t owner =
            position ? asset->position_constraints[i].entity : asset->aim_constraints[i - positions].entity;
        if (claims->protected_rows[owner] & SCENE_HINGE_TRANSFORM) {
            return hinge_conflict(asset, owner);
        }
        claim_transform(claims, owner, position ? ANIM_POSE_POSITION_FIELD : ANIM_POSE_ROTATION_FIELD);
    }
    for (int i = 0; i < asset->position_constraint_count; i++) {
        const scene_asset_position_constraint_t* row = &asset->position_constraints[i];
        const scene_row_t driver =
            protect_inputs(claims, row->entity, row->source_first, row->source_count, SCENE_ROW_NONE);
        if (driver != SCENE_ROW_NONE) {
            return driven_failure(claims, driver);
        }
    }
    for (int i = 0; i < asset->aim_constraint_count; i++) {
        const scene_asset_aim_constraint_t* row = &asset->aim_constraints[i];
        const scene_row_t driver =
            protect_inputs(claims, row->entity, row->source_first, row->source_count, row->world_up_object);
        if (driver != SCENE_ROW_NONE) {
            return driven_failure(claims, driver);
        }
    }
    return scene_failure_make(SCENE_OK, ASSET_OK, "");
}

int
scene_constraint_source_count(const scene_asset_t* asset, scene_row_t row, int component) {
    if (component == SCENE_COMPONENT_CPOS) {
        for (int i = 0; i < asset->position_constraint_count; i++) {
            if (asset->position_constraints[i].entity == row) {
                return asset->position_constraints[i].source_count;
            }
        }
    } else {
        for (int i = 0; i < asset->aim_constraint_count; i++) {
            if (asset->aim_constraints[i].entity == row) {
                return asset->aim_constraints[i].source_count;
            }
        }
    }
    return 0;
}

static void
open_inputs(const scene_asset_t* asset, scene_constraint_inputs_t* inputs, float weight, int first, int count) {
    inputs->weights.weight = weight;
    for (int i = 0; i < count; i++) {
        inputs->weights.values[CONSTRAINT_FIRST_SOURCE_FIELD + i] = asset->sources[first + i].weight;
        inputs->targets[i] = source_row(asset, first + i);
    }
}

void
scene_constraint_open(scene_t* scene) {
    for (int i = 0; i < scene->asset.position_constraint_count; i++) {
        const scene_asset_position_constraint_t* row = &scene->asset.position_constraints[i];
        scene->positions[i].own = scene->locals[row->entity].position;
        open_inputs(&scene->asset, &scene->positions[i].inputs, row->weight, row->source_first, row->source_count);
    }
    for (int i = 0; i < scene->asset.aim_constraint_count; i++) {
        const scene_asset_aim_constraint_t* row = &scene->asset.aim_constraints[i];
        scene_aim_constraint_t* aim = &scene->aims[i];
        open_inputs(&scene->asset, &aim->inputs, row->weight, row->source_first, row->source_count);
        aim->own = scene->locals[row->entity].rotation;
        aim->local_aim_inverse = quatf_conjugate(quatf_looking(row->aim_vector, row->up_vector));
    }
}

static float
weight_at(const scene_constraint_inputs_t* inputs, int field) {
    return mathf_clamp(inputs->weights.values[field], 0.0F, 1.0F);
}

static bool
weighted_position(const scene_t* scene, const scene_constraint_inputs_t* inputs, int count, vec3f_t offset,
                  rig_bind_t bind, vec3f_t from, vec3f_t* result) {
    float weights[SCENE_CONSTRAINT_SOURCES_MAX];
    float sum = 0.0F;
    for (int i = 0; i < count; i++) {
        weights[i] = weight_at(inputs, CONSTRAINT_FIRST_SOURCE_FIELD + i);
        sum += weights[i];
    }
    if (sum == 0.0F) {
        return false;
    }
    *result = vec3f_zero();
    for (int i = 0; i < count; i++) {
        if (weights[i] > 0.0F) {
            const transformf_t target = scene_row_world(scene->root, inputs->targets[i]);
            const vec3f_t point = vec3f_sub(rig_follow_position(&target, offset, bind), from);
            *result = vec3f_add(*result, vec3f_scale(point, weights[i] / sum));
        }
    }
    return true;
}

static void
apply_position(scene_t* scene, const scene_asset_position_constraint_t* row, const scene_position_constraint_t* c) {
    transformf_t* local = &scene->locals[row->entity];
    const float weight = weight_at(&c->inputs, CONSTRAINT_WEIGHT_FIELD);
    vec3f_t next = c->own, target;
    if (weight > 0.0F
        && weighted_position(scene, &c->inputs, row->source_count, row->translation_offset,
                             (rig_bind_t)row->binding_mode, vec3f_zero(), &target)) {
        const transformf_t frame = scene_row_parent_world(scene->root, row->entity);
        if (fabsf(frame.scale.x) >= FLT_MIN && fabsf(frame.scale.y) >= FLT_MIN && fabsf(frame.scale.z) >= FLT_MIN) {
            const vec3f_t mapped = transformf_inverse_point(&frame, target);
            const vec3f_t blended =
                weight == 1.0F ? mapped : vec3f_add(vec3f_scale(c->own, 1.0F - weight), vec3f_scale(mapped, weight));
            if (isfinite(blended.x) && isfinite(blended.y) && isfinite(blended.z)) {
                next = blended;
            }
        }
    }
    if (memcmp(&local->position, &next, sizeof next) != 0) {
        transformf_set_position(local, next);
        scene->flags[row->entity] |= SCENE_FLAG_PLACED;
    }
}

static vec3f_t
world_up(const scene_t* scene, const scene_asset_aim_constraint_t* row, vec3f_t from) {
    switch (row->world_up_type) {
        case SCENE_ASSET_WORLD_UP_VECTOR: return row->world_up_vector;
        case SCENE_ASSET_WORLD_UP_OBJECT_UP:
            return vec3f_sub(scene_row_world(scene->root, row->world_up_object).position, from);
        case SCENE_ASSET_WORLD_UP_OBJECT_ROTATION_UP:
            return quatf_rotate(scene_row_world(scene->root, row->world_up_object).rotation, row->world_up_vector);
        default: return vec3f_up();
    }
}

static quatf_t
aim_world_rotation(const scene_t* scene, const scene_asset_aim_constraint_t* row, const scene_aim_constraint_t* c,
                   vec3f_t from, vec3f_t direction, quatf_t own) {
    if (row->world_up_type == SCENE_ASSET_WORLD_UP_NONE) {
        const quatf_t swing = quatf_from_to(quatf_rotate(own, row->aim_vector), direction);
        return quatf_mul(swing, own);
    }
    return quatf_mul(quatf_looking(direction, world_up(scene, row, from)), c->local_aim_inverse);
}

static void
apply_aim(scene_t* scene, const scene_asset_aim_constraint_t* row, const scene_aim_constraint_t* c) {
    const float weight = weight_at(&c->inputs, CONSTRAINT_WEIGHT_FIELD);
    quatf_t next = c->own;
    if (weight > 0.0F) {
        const vec3f_t from = scene_row_world(scene->root, row->entity).position;
        vec3f_t direction;
        if (weighted_position(scene, &c->inputs, row->source_count, vec3f_zero(), RIG_BIND_WORLD, from, &direction)) {
            if (vec3f_dot(direction, direction) > AIM_DISTANCE_SQUARED_MIN) {
                const quatf_t frame = scene_row_parent_world(scene->root, row->entity).rotation;
                const quatf_t own = quatf_mul(frame, c->own);
                const quatf_t aimed = aim_world_rotation(scene, row, c, from, direction, own);
                const quatf_t offset = quatf_mul(aimed, row->rotation_offset);
                next = quatf_nlerp(c->own, quatf_mul(quatf_conjugate(frame), offset), weight);
            }
        }
    }
    transformf_t* local = &scene->locals[row->entity];
    if (memcmp(&local->rotation, &next, sizeof next) != 0) {
        transformf_set_rotation(local, next);
        scene->flags[row->entity] |= SCENE_FLAG_PLACED;
    }
}

static bool
path_placed(const scene_t* scene, scene_row_t row) {
    for (; row != SCENE_ROW_NONE; row = scene_row_parent(scene, row)) {
        if (scene->flags[row] & SCENE_FLAG_PENDING_PLACEMENT) {
            return true;
        }
    }
    return false;
}

static bool
inputs_placed(const scene_t* scene, const scene_constraint_inputs_t* inputs, scene_row_t owner, int count) {
    for (int i = 0; i < count; i++) {
        if (weight_at(inputs, CONSTRAINT_FIRST_SOURCE_FIELD + i) > 0.0F && path_placed(scene, inputs->targets[i])) {
            return true;
        }
    }
    return path_placed(scene, owner);
}

void
scene_constraint_step(scene_t* scene, bool applied) {
    for (int i = 0; i < scene->asset.position_constraint_count; i++) {
        const scene_asset_position_constraint_t* row = &scene->asset.position_constraints[i];
        const scene_position_constraint_t* c = &scene->positions[i];
        if (applied || inputs_placed(scene, &c->inputs, row->entity, row->source_count)) {
            apply_position(scene, row, c);
        }
    }
    for (int i = 0; i < scene->asset.aim_constraint_count; i++) {
        const scene_asset_aim_constraint_t* row = &scene->asset.aim_constraints[i];
        const scene_aim_constraint_t* c = &scene->aims[i];
        if (applied || inputs_placed(scene, &c->inputs, row->entity, row->source_count)
            || path_placed(scene, row->world_up_object)) {
            apply_aim(scene, row, c);
        }
    }
}

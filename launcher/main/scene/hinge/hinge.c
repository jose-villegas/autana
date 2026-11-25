#include "scene/hinge/hinge.h"

#include <string.h>

#include "scene/scene_internal.h"

static const anim_field_t FIELDS[] = {
    ANIM_FIELD(scene_hinge_t, angle, ANIM_VALUE_FLOAT),
};

const anim_component_fields_t HINGE_FIELDS = {ANIM_COMPONENT_HINGE, FIELDS, HINGE_FIELD_COUNT};

void
scene_hinge_open(scene_t* scene) {
    for (int i = 0; i < scene->asset.hinge_count; i++) {
        const scene_asset_hinge_t* row = &scene->asset.hinges[i];
        scene->hinges[i] = (scene_hinge_t){
            .joint = {.rest = scene->locals[row->entity], .axis = row->axis, .min = row->min, .max = row->max},
            .angle = row->angle,
        };
    }
}

int
scene_hinge_index(const scene_asset_t* asset, scene_row_t row) {
    for (int i = 0; i < asset->hinge_count; i++) {
        if (asset->hinges[i].entity == row) {
            return i;
        }
    }
    return -1;
}

static void
apply_pose(const scene_hinge_t* hinge, float angle, transformf_t* local) {
    const transformf_t pose = hinge_pose(&hinge->joint, angle);
    local->position = pose.position;
    local->rotation = pose.rotation;
    local->cached = false;
}

void
scene_hinge_step(scene_t* scene, bool applied) {
    for (int i = 0; i < scene->asset.hinge_count; i++) {
        const scene_row_t row = scene->asset.hinges[i].entity;
        if (!applied && !(scene->flags[row] & SCENE_FLAG_PENDING_PLACEMENT)) {
            continue;
        }
        const scene_hinge_t* hinge = &scene->hinges[i];
        const transformf_t* local = &scene->locals[row];
        transformf_t pose = *local;
        apply_pose(hinge, hinge->angle, &pose);
        if (memcmp(&local->rotation, &pose.rotation, sizeof pose.rotation) != 0
            || memcmp(&local->position, &pose.position, sizeof pose.position) != 0) {
            scene_row_set_local(scene->root, row, pose);
        }
    }
}

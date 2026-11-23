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

void
scene_hinge_step(scene_t* scene) {
    for (int i = 0; i < scene->asset.hinge_count; i++) {
        const scene_row_t row = scene->asset.hinges[i].entity;
        const scene_hinge_t* hinge = &scene->hinges[i];
        const transformf_t* local = &scene->locals[row];
        transformf_t pose = hinge_pose(&hinge->joint, hinge->angle);
        if (memcmp(&local->rotation, &pose.rotation, sizeof pose.rotation) != 0
            || memcmp(&local->position, &pose.position, sizeof pose.position) != 0) {
            pose.scale = local->scale;
            scene_row_set_local(scene->root, row, pose);
        }
    }
}

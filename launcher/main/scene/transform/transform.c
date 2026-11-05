/* transform: the transform component's calls; scene_hierarchy.c keeps the worlds. */
#include "scene/transform/transform.h"

#include "scene/scene_internal.h"

transformf_t
transform_local(entity_t entity) {
    scene_row_t row;
    const scene_root_t* record = scene_entity_lookup(entity, &row);
    return record == NULL ? transformf_identity() : scene_row_local(record, row);
}

void
transform_set_local(entity_t entity, transformf_t local) {
    scene_row_t row;
    scene_root_t* record = scene_entity_lookup(entity, &row);
    if (record != NULL) {
        scene_row_set_local(record, row, local);
    }
}

transformf_t
transform_world(entity_t entity) {
    scene_row_t row;
    const scene_root_t* record = scene_entity_lookup(entity, &row);
    return record == NULL ? transformf_identity() : scene_row_world(record, row);
}

void
transform_set_world(entity_t entity, transformf_t world) {
    scene_row_t row;
    scene_root_t* record = scene_entity_lookup(entity, &row);
    if (record != NULL) {
        scene_row_set_world(record, row, world);
    }
}

mat4f_t
transform_world_matrix(entity_t entity) {
    scene_row_t row;
    const scene_root_t* record = scene_entity_lookup(entity, &row);
    return record == NULL ? mat4f_identity() : scene_row_world_matrix(record, row);
}

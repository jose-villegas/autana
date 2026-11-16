/* transform: the transform component's calls; scene_hierarchy.c keeps the worlds. */
#include "scene/transform/transform.h"

#include "scene/scene_internal.h"

typedef transformf_t (*row_read_fn)(const scene_root_t* record, scene_row_t row);
typedef void (*row_write_fn)(scene_root_t* record, scene_row_t row, transformf_t value);

/* `read_row` of a live entity; the identity for a refused one. */
static transformf_t
get(entity_t entity, row_read_fn read_row) {
    scene_row_t row;
    const scene_root_t* record = scene_entity_lookup(entity, &row);
    return record == NULL ? transformf_identity() : read_row(record, row);
}

/* `write_row` to a live entity; nothing for a refused one. */
static void
put(entity_t entity, transformf_t value, row_write_fn write_row) {
    scene_row_t row;
    scene_root_t* record = scene_entity_lookup(entity, &row);
    if (record != NULL) {
        write_row(record, row, value);
    }
}

transformf_t
transform_local(entity_t entity) {
    return get(entity, scene_row_local);
}

void
transform_set_local(entity_t entity, transformf_t local) {
    put(entity, local, scene_row_set_local);
}

transformf_t
transform_world(entity_t entity) {
    return get(entity, scene_row_world);
}

void
transform_set_world(entity_t entity, transformf_t world) {
    put(entity, world, scene_row_set_world);
}

mat4f_t
transform_world_matrix(entity_t entity) {
    scene_row_t row;
    const scene_root_t* record = scene_entity_lookup(entity, &row);
    return record == NULL ? mat4f_identity() : scene_row_world_matrix(record, row);
}

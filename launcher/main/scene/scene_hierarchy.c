/*
 * scene_hierarchy: a parent per entity, from the SCNE entity_parents part.
 * A parent always comes before its children, so one forward pass sees every
 * parent's world and enabled state before its children's. Worlds are kept
 * only for entities with children; a renderer's model is its own world.
 */
#include <assert.h>
#include <stdalign.h>
#include <string.h>

#include "scene/scene_internal.h"

_Static_assert(SCENE_WORLD_SLOT_NONE == 0xFFFFU, "world_slots are cleared with 0xFF bytes");

static scene_entity_t
parent_of(const scene_t* scene, scene_entity_t entity) {
    const uint16_t parent = scene->asset.parents[entity];
    return parent == SCENE_ASSET_PARENT_ROOT ? SCENE_ENTITY_NONE : (scene_entity_t)parent;
}

bool
scene_hierarchy_open(scene_t* scene) {
    const int entities = scene->asset.entity_count;
    memset(scene->world_slots, 0xFF, sizeof *scene->world_slots * (size_t)entities);
    int worlds_needed = 0;
    for (int e = 0; e < entities; e++) {
        const scene_entity_t parent = parent_of(scene, (scene_entity_t)e);
        if (parent != SCENE_ENTITY_NONE && scene->world_slots[parent] == SCENE_WORLD_SLOT_NONE) {
            scene->world_slots[parent] = 0; /* has children; numbered below, in entity order */
            worlds_needed++;
        }
    }
    if (worlds_needed == 0) {
        return true;
    }
    size_t end = 0;
    const size_t worlds = scene_storage_carve(&end, sizeof(mat4f_t) * (size_t)worlds_needed, alignof(mat4f_t));
    const size_t owners =
        scene_storage_carve(&end, sizeof(scene_entity_t) * (size_t)worlds_needed, alignof(scene_entity_t));
    uint8_t* block = scene_allocate_kept(end);
    if (block == NULL) {
        return false;
    }
    scene->worlds = (mat4f_t*)(void*)(block + worlds);
    scene->world_entities = (scene_entity_t*)(void*)(block + owners);
    for (int e = 0; e < entities; e++) {
        if (scene->world_slots[e] != SCENE_WORLD_SLOT_NONE) {
            scene->world_entities[scene->world_count] = (scene_entity_t)e;
            scene->world_slots[e] = (uint16_t)scene->world_count++;
        }
    }
    return true;
}

void
scene_hierarchy_release(scene_t* scene) {
    scene_free_kept(scene->worlds);
    scene->worlds = NULL;
}

static bool
changed(const scene_t* scene, scene_entity_t entity) {
    const scene_entity_t parent = parent_of(scene, entity);
    return (scene->flags[entity] & SCENE_FLAG_DIRTY) != 0
           || (parent != SCENE_ENTITY_NONE && (scene->flags[parent] & SCENE_FLAG_MOVED) != 0);
}

/* The parent's kept world times the entity's own transform. */
static mat4f_t
world_from_parent(const scene_t* scene, scene_entity_t entity) {
    const mat4f_t local = transformf_compute_matrix(&scene->locals[entity]);
    const scene_entity_t parent = parent_of(scene, entity);
    return parent == SCENE_ENTITY_NONE ? local : mat4f_mul_affine(scene->worlds[scene->world_slots[parent]], local);
}

void
scene_resolve(scene_t* scene) {
    for (int i = 0; i < scene->world_count; i++) {
        const scene_entity_t e = scene->world_entities[i];
        if (changed(scene, e)) {
            scene->worlds[i] = world_from_parent(scene, e);
            scene->flags[e] |= SCENE_FLAG_MOVED;
        }
    }
    for (int i = 0; i < scene->renderer_count; i++) {
        scene_renderer_t* r = &scene->renderers[i];
        if (changed(scene, r->entity)) {
            const uint16_t slot = scene->world_slots[r->entity];
            r->model = slot != SCENE_WORLD_SLOT_NONE ? scene->worlds[slot] : world_from_parent(scene, r->entity);
            r->placed = !scene_transform_is_identity(&r->model);
        }
    }
    for (int i = 0; i < scene->asset.entity_count; i++) {
        scene->flags[i] &= (uint8_t)~(SCENE_FLAG_DIRTY | SCENE_FLAG_MOVED);
    }
}

void
scene_hierarchy_update_active(scene_t* scene, scene_entity_t entity) {
    for (int e = entity; e < scene->asset.entity_count; e++) {
        const scene_entity_t parent = parent_of(scene, (scene_entity_t)e);
        const bool active = (scene->flags[e] & SCENE_FLAG_ENABLED) != 0
                            && (parent == SCENE_ENTITY_NONE || (scene->flags[parent] & SCENE_FLAG_ACTIVE) != 0);
        if (active != ((scene->flags[e] & SCENE_FLAG_ACTIVE) != 0)) {
            scene->flags[e] ^= SCENE_FLAG_ACTIVE;
            scene->flags[e] |= SCENE_FLAG_CHANGED;
        }
    }
}

scene_entity_t
scene_entity_parent(const scene_t* scene, scene_entity_t entity) {
    assert(entity < scene->asset.entity_count);
    return parent_of(scene, entity);
}

bool
scene_entity_is_within(const scene_t* scene, scene_entity_t entity, scene_entity_t ancestor) {
    assert(entity < scene->asset.entity_count);
    for (scene_entity_t e = entity; e != SCENE_ENTITY_NONE && e >= ancestor; e = parent_of(scene, e)) {
        if (e == ancestor) {
            return true;
        }
    }
    return false;
}

bool
scene_entity_active(const scene_t* scene, scene_entity_t entity) {
    assert(entity < scene->asset.entity_count);
    return (scene->flags[entity] & SCENE_FLAG_ACTIVE) != 0;
}

transformf_t
scene_entity_world(const scene_t* scene, scene_entity_t entity) {
    assert(entity < scene->asset.entity_count);
    transformf_t world = scene->locals[entity];
    for (scene_entity_t e = parent_of(scene, entity); e != SCENE_ENTITY_NONE; e = parent_of(scene, e)) {
        world = transformf_compose(&scene->locals[e], &world);
    }
    return world;
}

void
scene_entity_set_world(scene_t* scene, scene_entity_t entity, transformf_t world) {
    assert(entity < scene->asset.entity_count);
    const scene_entity_t parent = parent_of(scene, entity);
    if (parent == SCENE_ENTITY_NONE) {
        scene_entity_set_local(scene, entity, world);
        return;
    }
    const transformf_t parent_world = scene_entity_world(scene, parent);
    scene_entity_set_local(scene, entity, transformf_relative(&parent_world, &world));
}

mat4f_t
scene_entity_world_matrix(const scene_t* scene, scene_entity_t entity) {
    assert(entity < scene->asset.entity_count);
    mat4f_t world = transformf_compute_matrix(&scene->locals[entity]);
    for (scene_entity_t e = parent_of(scene, entity); e != SCENE_ENTITY_NONE; e = parent_of(scene, e)) {
        world = mat4f_mul_affine(transformf_compute_matrix(&scene->locals[e]), world);
    }
    return world;
}

bool
scene_entity_is_plain_root(const scene_t* scene, scene_entity_t entity) {
    return parent_of(scene, entity) == SCENE_ENTITY_NONE && !transformf_mirrors(&scene->locals[entity]);
}

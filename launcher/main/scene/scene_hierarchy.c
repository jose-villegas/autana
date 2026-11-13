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

static scene_row_t
parent_of(const scene_t* scene, scene_row_t entity) {
    const uint16_t parent = scene->asset.parents[entity];
    return parent == SCENE_ASSET_PARENT_ROOT ? SCENE_ROW_NONE : (scene_row_t)parent;
}

bool
scene_hierarchy_open(scene_t* scene) {
    const int entities = scene->asset.entity_count;
    memset(scene->world_slots, 0xFF, sizeof *scene->world_slots * (size_t)entities);
    int worlds_needed = 0;
    for (int e = 0; e < entities; e++) {
        const scene_row_t parent = parent_of(scene, (scene_row_t)e);
        if (parent != SCENE_ROW_NONE && scene->world_slots[parent] == SCENE_WORLD_SLOT_NONE) {
            scene->world_slots[parent] = 0; /* has children; numbered below, in entity order */
            worlds_needed++;
        }
    }
    if (worlds_needed == 0) {
        return true;
    }
    size_t end = 0;
    const size_t worlds = scene_storage_carve(&end, sizeof(mat4f_t) * (size_t)worlds_needed, alignof(mat4f_t));
    const size_t owners = scene_storage_carve(&end, sizeof(scene_row_t) * (size_t)worlds_needed, alignof(scene_row_t));
    uint8_t* block = scene_allocate_kept(end);
    if (block == NULL) {
        return false;
    }
    scene->worlds = (mat4f_t*)(void*)(block + worlds);
    scene->world_entities = (scene_row_t*)(void*)(block + owners);
    for (int e = 0; e < entities; e++) {
        if (scene->world_slots[e] != SCENE_WORLD_SLOT_NONE) {
            scene->world_entities[scene->world_count] = (scene_row_t)e;
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
changed(const scene_t* scene, scene_row_t entity) {
    const scene_row_t parent = parent_of(scene, entity);
    return (scene->flags[entity] & SCENE_FLAG_DIRTY) != 0
           || (parent != SCENE_ROW_NONE && (scene->flags[parent] & SCENE_FLAG_MOVED) != 0);
}

/* The parent's kept world, or the root's, times the entity's own transform. */
static mat4f_t
world_from_parent(const scene_t* scene, scene_row_t entity) {
    const mat4f_t local = transformf_compute_matrix(&scene->locals[entity]);
    const scene_row_t parent = parent_of(scene, entity);
    if (parent == SCENE_ROW_NONE) {
        return scene->root->placed ? mat4f_mul_affine(scene->root->world, local) : local;
    }
    return mat4f_mul_affine(scene->worlds[scene->world_slots[parent]], local);
}

/* The box of every chunk's sphere of batch `batch`, where `model` puts it.
 * Not inlined: a renderer with no batch keeps its frame. */
static __attribute__((noinline)) box3f_t
batch_world_bounds(const r3d_batch_t* batch, const mat4f_t* model) {
    box3f_t local = box3f_empty();
    for (int c = 0; c < batch->chunk_count; c++) {
        const spheref_t s = r3d_batch_chunk_sphere(batch, c);
        const vec3f_t reach = {s.radius, s.radius, s.radius};
        local = box3f_union(local, (box3f_t){vec3f_sub(s.centre, reach), vec3f_add(s.centre, reach)});
    }
    return r3d_box_placed(local, model);
}

void
scene_renderer_bound(scene_renderer_t* r) {
    r->changed = true;
    if (r->mesh.node_count == 0) {
        return;
    }
    const mat4f_t* model = r->placed ? &r->model : NULL;
    r->world_bounds =
        r->batch == NULL ? r3d_lit_mesh_world_bounds(&r->mesh, model) : batch_world_bounds(r->batch, model);
}

static void
list_active_renderers(scene_t* scene) {
    scene->active_renderer_count = 0;
    for (int i = 0; i < scene->renderer_count; i++) {
        const scene_row_t e = scene->renderers[i].entity;
        /* A skin settled without its buffers did not fit: it is not drawn. */
        const bool starved = scene->skinned_renderers[e].starved;
        if ((scene->flags[e] & SCENE_FLAG_ACTIVE) != 0 && !starved) {
            scene->active_renderers[scene->active_renderer_count++] = (uint16_t)i;
        }
    }
    scene->active_renderers_stale = false;
}

void
scene_resolve(scene_t* scene) {
    scene_skins_settle(scene);
    if (scene->active_renderers_stale) {
        list_active_renderers(scene);
    }
    for (int i = 0; i < scene->world_count; i++) {
        const scene_row_t e = scene->world_entities[i];
        if (changed(scene, e)) {
            scene->worlds[i] = world_from_parent(scene, e);
            scene->flags[e] |= SCENE_FLAG_MOVED;
        }
    }
    for (int i = 0; i < scene->active_renderer_count; i++) {
        scene_renderer_t* r = &scene->renderers[scene->active_renderers[i]];
        if (changed(scene, r->entity)) {
            const uint16_t slot = scene->world_slots[r->entity];
            r->model = slot != SCENE_WORLD_SLOT_NONE ? scene->worlds[slot] : world_from_parent(scene, r->entity);
            r->placed = !scene_transform_is_identity(&r->model);
            scene_renderer_bound(r);
        }
    }
    for (int i = 0; i < scene->asset.entity_count; i++) {
        scene->flags[i] &= (uint8_t)~(SCENE_FLAG_DIRTY | SCENE_FLAG_MOVED);
    }
}

void
scene_hierarchy_update_active(scene_t* scene, scene_row_t entity) {
    const bool root_active = (scene->root->flags & SCENE_FLAG_ACTIVE) != 0;
    for (int e = entity == SCENE_ROW_ROOT ? 0 : entity; e < scene->asset.entity_count; e++) {
        const scene_row_t parent = parent_of(scene, (scene_row_t)e);
        const bool active =
            (scene->flags[e] & SCENE_FLAG_ENABLED) != 0
            && (parent == SCENE_ROW_NONE ? root_active : (scene->flags[parent] & SCENE_FLAG_ACTIVE) != 0);
        if (active != ((scene->flags[e] & SCENE_FLAG_ACTIVE) != 0)) {
            scene->flags[e] ^= SCENE_FLAG_ACTIVE;
            scene->flags[e] |= SCENE_FLAG_CHANGED;
            scene->active_renderers_stale = true;
            /* A skin holds its buffers only while ACTIVE. */
            scene->skins_pending |= active && scene->skinned_renderers[e].bound;
            if (!active) {
                scene_skin_deactivate(scene, (scene_row_t)e);
            }
        }
    }
}

scene_row_t
scene_row_parent(const scene_t* scene, scene_row_t row) {
    return row == SCENE_ROW_ROOT ? SCENE_ROW_NONE : parent_of(scene, row);
}

void
scene_row_set_local(scene_root_t* root, scene_row_t row, transformf_t local) {
    scene_t* scene = root->scene;
    if (row == SCENE_ROW_ROOT) {
        /* The root is every top-level row's parent: the rule below holds for it too. */
        assert(transformf_scale_is_even(&local));
        root->local = local;
        root->world = transformf_compute_matrix(&local);
        root->placed = !scene_transform_is_identity(&root->world);
        for (int e = 0; scene != NULL && e < scene->asset.entity_count; e++) {
            if (parent_of(scene, (scene_row_t)e) == SCENE_ROW_NONE) {
                scene->flags[e] |= SCENE_FLAG_PLACED;
            }
        }
        return;
    }
    /* A parent scales evenly, so every world below it is an exact TRS; the
     * loader and the importer keep content so. */
    assert(scene->world_slots[row] == SCENE_WORLD_SLOT_NONE || transformf_scale_is_even(&local));
    scene->locals[row] = local;
    scene->flags[row] |= SCENE_FLAG_PLACED;
}

transformf_t
scene_row_local(const scene_root_t* root, scene_row_t row) {
    return row == SCENE_ROW_ROOT ? root->local : root->scene->locals[row];
}

transformf_t
scene_row_world(const scene_root_t* root, scene_row_t row) {
    transformf_t world = scene_row_local(root, row);
    if (row == SCENE_ROW_ROOT) {
        return world;
    }
    for (scene_row_t e = parent_of(root->scene, row); e != SCENE_ROW_NONE; e = parent_of(root->scene, e)) {
        world = transformf_compose(&root->scene->locals[e], &world);
    }
    return transformf_compose(&root->local, &world);
}

mat4f_t
scene_row_world_matrix(const scene_root_t* root, scene_row_t row) {
    const transformf_t local = scene_row_local(root, row);
    mat4f_t world = transformf_compute_matrix(&local);
    if (row == SCENE_ROW_ROOT) {
        return world;
    }
    for (scene_row_t e = parent_of(root->scene, row); e != SCENE_ROW_NONE; e = parent_of(root->scene, e)) {
        world = mat4f_mul_affine(transformf_compute_matrix(&root->scene->locals[e]), world);
    }
    return root->placed ? mat4f_mul_affine(root->world, world) : world;
}

void
scene_row_set_world(scene_root_t* root, scene_row_t row, transformf_t world) {
    const scene_row_t parent = scene_row_parent(root->scene, row);
    if (row == SCENE_ROW_ROOT) {
        scene_row_set_local(root, row, world);
        return;
    }
    const transformf_t parent_world = scene_row_world(root, parent == SCENE_ROW_NONE ? SCENE_ROW_ROOT : parent);
    scene_row_set_local(root, row, transformf_relative(&parent_world, &world));
}

uint8_t
scene_row_flags(const scene_root_t* root, scene_row_t row) {
    return row == SCENE_ROW_ROOT ? root->flags : root->scene->flags[row];
}

void
scene_row_set_enabled(scene_root_t* root, scene_row_t row, bool enabled) {
    if (row == SCENE_ROW_ROOT) {
        root->flags = enabled ? (uint8_t)(root->flags | SCENE_FLAG_ENABLED | SCENE_FLAG_ACTIVE)
                              : (uint8_t)(root->flags & ~(SCENE_FLAG_ENABLED | SCENE_FLAG_ACTIVE));
    } else {
        uint8_t* flags = &root->scene->flags[row];
        *flags = enabled ? (uint8_t)(*flags | SCENE_FLAG_ENABLED) : (uint8_t)(*flags & ~SCENE_FLAG_ENABLED);
        *flags |= SCENE_FLAG_CHANGED;
    }
    if (root->scene != NULL) {
        scene_hierarchy_update_active(root->scene, row);
    }
}

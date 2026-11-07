/* renderer: an entity's mesh, and the skin that deforms it. */
#include "scene/renderer/renderer.h"

#include <string.h>

#include "core/memory.h"
#include "scene/scene_internal.h"

/* The index of the entity's renderer in its scene `*scene`, or -1. */
static int
renderer_of(entity_t entity, scene_t** scene) {
    scene_row_t row;
    *scene = scene_entity_row(entity, &row);
    return *scene == NULL ? -1 : scene_renderer_index(*scene, row);
}

const char*
renderer_mesh_id(entity_t entity) {
    scene_t* scene;
    const int i = renderer_of(entity, &scene);
    if (i < 0) {
        return NULL;
    }
    return i < scene->asset.renderer_count ? scene->asset.renderers[i].mesh
                                           : scene->asset.skinned_renderers[i - scene->asset.renderer_count].mesh;
}

const r3d_lit_mesh_t*
renderer_mesh(entity_t entity) {
    scene_t* scene;
    const int i = renderer_of(entity, &scene);
    if (i < 0) {
        return NULL;
    }
    scene_skins_settle(scene);
    return &scene->renderers[i].mesh;
}

bool
renderer_set_mesh(entity_t entity, const r3d_lit_mesh_t* mesh) {
    scene_t* scene;
    const int i = renderer_of(entity, &scene);
    if (i < 0 || i >= scene->asset.renderer_count) {
        return false;
    }
    scene_renderer_t* r = &scene->renderers[i];
    if (R3D_LOD_WORDS(mesh->lod_group_count) > r->lod_words) {
        return false;
    }
    r->mesh = *mesh;
    scene_renderer_bound(r);
    /* Another mesh's groups are not these bits': start it at its finest. */
    if (r->lod_coarse != NULL) {
        memset(r->lod_coarse, 0, sizeof(uint32_t) * r->lod_words);
    }
    return true;
}

static asset_status_t
open_renderer_skin(scene_t* scene, scene_row_t entity, int renderer, const char* id, scene_skinned_renderer_t* next) {
    asset_status_t status = r3d_skin_open(scene->pack, id, &next->skin);
    const scene_animator_t* animator = &scene->animators[entity];
    const scene_skinned_renderer_t* old = &scene->skinned_renderers[entity];
    next->bind = old->bound ? old->bind : scene->renderers[renderer].mesh;
    if (status == ASSET_OK
        && (next->skin.joint_count != animator->skeleton.joint_count
            || next->skin.vertex_count != (uint32_t)next->bind.vertex_count)) {
        status = ASSET_ERR_FORMAT;
    }
    next->light_mode = (skin_light_mode_t)next->skin.lighting_mode;
    next->bound = true;
    return status;
}

bool
scene_renderer_bind_skin(scene_t* scene, scene_row_t row, const char* id, bool take, scene_failure_t* why) {
    const int renderer = scene_renderer_index(scene, row);
    if (renderer < 0) {
        return scene_report(why, SCENE_ERR_ASSET, ASSET_ERR_NOT_FOUND, id);
    }
    /* Its normals are lit in its own frame, so a skinned renderer is a root. */
    if (scene_row_parent(scene, row) != SCENE_ROW_NONE) {
        return scene_report(why, SCENE_ERR_ASSET, ASSET_ERR_FORMAT, id);
    }
    scene_skinned_renderer_t next = {0};
    const asset_status_t status = open_renderer_skin(scene, row, renderer, id, &next);
    if (status != ASSET_OK) {
        return scene_report(why, SCENE_ERR_ASSET, status, id);
    }
    const bool lit = scene->light_count > 0 || scene->ambient_intensity > 0.0F;
    if (lit && next.bind.colors == NULL) {
        return scene_report(why, SCENE_ERR_ASSET, ASSET_ERR_FORMAT, SCENE_LIGHT_FAILURE_ALBEDO);
    }
    r3d_skin_light_transfer_t* transfer = NULL;
    if (lit && scene->light_transfer == NULL) {
        transfer = scene_light_transfer_take(scene);
        if (transfer == NULL) {
            return scene_report(why, SCENE_ERR_MEMORY, ASSET_OK, id);
        }
        scene->light_transfer = transfer;
    }
    take = take && (scene->flags[row] & SCENE_FLAG_ACTIVE) != 0;
    if (take && !scene_skin_take(scene, row, &next)) {
        if (transfer != NULL) {
            memory_free(transfer);
            scene->light_transfer = NULL;
        }
        return scene_report(why, SCENE_ERR_MEMORY, ASSET_OK, id);
    }
    scene_skinned_renderer_t* slot = &scene->skinned_renderers[row];
    scene_skin_give(scene, slot);
    if (slot->starved) {
        scene->starved_count--;
    }
    *slot = next;
    scene_skin_show(scene, row);
    scene->skins_pending |= !take;
    return scene_report(why, SCENE_OK, ASSET_OK, id);
}

bool
renderer_set_skin(entity_t entity, const char* id, scene_failure_t* why) {
    scene_row_t row;
    scene_t* scene = scene_entity_row(entity, &row);
    if (scene == NULL) {
        return scene_report(why, SCENE_ERR_ASSET, ASSET_ERR_NOT_FOUND, id);
    }
    return scene_renderer_bind_skin(scene, row, id, true, why);
}

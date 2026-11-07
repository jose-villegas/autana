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
    return i < 0 ? NULL : &scene->renderers[i].mesh;
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
    next->bind = old->internal_storage == NULL ? scene->renderers[renderer].mesh : old->bind;
    if (status == ASSET_OK
        && (next->skin.joint_count != animator->skeleton.joint_count
            || next->skin.vertex_count != (uint32_t)next->bind.vertex_count)) {
        status = ASSET_ERR_FORMAT;
    }
    return status;
}

static bool
prepare_skin_lighting(scene_t* scene, scene_row_t entity, scene_skinned_renderer_t* next, bool lit) {
    r3d_skin_light_transfer_t* transfer = NULL;
    if (lit && scene->light_transfer == NULL) {
        transfer = scene_light_transfer_take(scene);
        if (transfer == NULL) {
            return false;
        }
    }
    if (!scene_skin_light_apply_mode(scene, entity, next, (skin_light_mode_t)next->skin.lighting_mode)) {
        memory_free(transfer);
        return false;
    }
    if (transfer != NULL) {
        scene->light_transfer = transfer;
    }
    return true;
}

static void
attach_renderer_skin(scene_t* scene, scene_row_t entity, int renderer, const scene_skinned_renderer_t* next) {
    scene_skin_light_table_release(scene, entity);
    memory_free(scene->skinned_renderers[entity].light_storage);
    memory_free(scene->skinned_renderers[entity].internal_storage);
    scene->skinned_renderers[entity] = *next;
    scene->renderers[renderer].mesh = next->output.mesh;
    if (scene->light_transfer != NULL) {
        scene->skinned_renderers[entity].output.mesh.colors = next->colors;
        scene->renderers[renderer].mesh.colors = next->colors;
    }
    scene->flags[entity] |= SCENE_FLAG_SKIN_DIRTY;
}

bool
renderer_set_skin(entity_t entity, const char* id, scene_failure_t* why) {
    scene_row_t row;
    scene_t* scene = scene_entity_row(entity, &row);
    if (scene == NULL) {
        return scene_report(why, SCENE_ERR_ASSET, ASSET_ERR_NOT_FOUND, id);
    }
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
    const size_t size = r3d_skin_output_bytes(&next.bind);
    size_t end = size;

    struct {
        size_t model, palette;
    } layout;

    SCENE_STORAGE_RESERVE(layout, end, model, mat4f_t, next.skin.joint_count);
    SCENE_STORAGE_RESERVE(layout, end, palette, mat4f_t, next.skin.joint_count);
    next.internal_storage = memory_alloc(end, MEMORY_INTERNAL);
    if (next.internal_storage == NULL) {
        return scene_report(why, SCENE_ERR_MEMORY, ASSET_OK, id);
    }
    if (lit && !scene_skin_light_allocate(&next)) {
        memory_free(next.internal_storage);
        return scene_report(why, SCENE_ERR_MEMORY, ASSET_OK, id);
    }
    (void)r3d_skin_output_init(&next.bind, next.internal_storage, size, &next.output);
    next.model = (mat4f_t*)(void*)((uint8_t*)next.internal_storage + layout.model);
    next.palette = (mat4f_t*)(void*)((uint8_t*)next.internal_storage + layout.palette);
    const scene_animator_t* animator = &scene->animators[row];
    anim_pose_model(&animator->skeleton, &animator->layer.shown, next.model);
    r3d_skin_palette(&next.skin, next.model, next.bind.position_scale, next.palette);
    r3d_skin_clusters(&next.skin, &next.bind, next.palette, 0, next.bind.cluster_count, &next.output);
    r3d_skin_refit_nodes(&next.output);
    if (!prepare_skin_lighting(scene, row, &next, lit)) {
        memory_free(next.light_storage);
        memory_free(next.internal_storage);
        return scene_report(why, SCENE_ERR_MEMORY, ASSET_OK, id);
    }
    attach_renderer_skin(scene, row, renderer, &next);
    return scene_report(why, SCENE_OK, ASSET_OK, id);
}

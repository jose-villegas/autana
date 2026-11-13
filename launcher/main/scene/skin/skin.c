/* skin: each skin's buffers, held only while its entity is active; its
 * colour and normal-palette block, and how it is lit. */
#include "scene/skin/skin.h"

#include <stdio.h>
#include <string.h>

#include "core/memory.h"
#include "scene/scene_internal.h"

bool
scene_skin_light_allocate(scene_skinned_renderer_t* skin) {
    size_t end = 0;

    struct {
        size_t colors, normals;
    } layout;

    SCENE_STORAGE_RESERVE(layout, end, colors, uint8_t, skin->bind.vertex_count* R3D_SKIN_LIGHT_CHANNELS);
    SCENE_STORAGE_RESERVE(layout, end, normals, mat3f_t, skin->skin.joint_count);
    void* storage = scene_allocate(end, MEMORY_FAST);
    if (storage == NULL) {
        return false;
    }
    memset(storage, 0, end);
    skin->light_storage = storage;
    skin->colors = (uint8_t (*)[R3D_SKIN_LIGHT_CHANNELS])(void*)((uint8_t*)storage + layout.colors);
    skin->normal_palette = (mat3f_t*)(void*)((uint8_t*)storage + layout.normals);
    return true;
}

void
scene_skin_light_prepare(scene_t* scene, scene_row_t entity) {
    if (scene->light_transfer == NULL) {
        return;
    }
    scene_skinned_renderer_t* skin = &scene->skinned_renderers[entity];
    skin->lighting_valid = r3d_skin_normal_palette(skin->palette, skin->skin.joint_count,
                                                   scene_row_world_matrix(scene->root, entity), skin->normal_palette);
    if (!skin->lighting_valid) {
        memset(skin->colors, 0, (size_t)skin->bind.vertex_count * R3D_SKIN_LIGHT_CHANNELS);
        (void)fprintf(stderr, "scene: %s: %s\n", SCENE_LIGHT_FAILURE_NORMAL, scene->asset.names[entity].name);
    }
}

/* A skin holding its buffers counts as a user of the scene's light table
 * while its mode is SKIN_LIGHT_TABLE; the first user allocates it. */
static bool
light_table_join(scene_t* scene) {
    if (scene->light_table == NULL) {
        r3d_skin_light_table_t* table = scene_allocate(sizeof *table, MEMORY_INTERNAL);
        if (table == NULL) {
            return false;
        }
        scene->light_table = table;
        scene->light_table_dirty = true;
    }
    scene->light_table_users++;
    return true;
}

bool
scene_skin_light_apply_mode(scene_t* scene, scene_row_t entity, scene_skinned_renderer_t* skin,
                            skin_light_mode_t mode) {
    if (!skin->bound) {
        return false;
    }
    if (skin->light_mode == mode) {
        return true;
    }
    if (skin->storage != NULL) {
        if (mode == SKIN_LIGHT_TABLE && !light_table_join(scene)) {
            return false;
        }
        scene_skin_light_table_release(scene, skin);
    }
    skin->light_mode = mode;
    scene->flags[entity] |= SCENE_FLAG_SKIN_DIRTY;
    return true;
}

void
scene_skin_light_table_release(scene_t* scene, const scene_skinned_renderer_t* skin) {
    if (skin->storage == NULL || skin->light_mode != SKIN_LIGHT_TABLE) {
        return;
    }
    scene->light_table_users--;
    if (scene->light_table_users == 0) {
        memory_free(scene->light_table);
        scene->light_table = NULL;
        scene->world_lights.table = NULL;
    }
}

bool
scene_skin_take(scene_t* scene, scene_row_t entity, scene_skinned_renderer_t* skin) {
    const size_t size = r3d_skin_output_bytes(&skin->bind);
    size_t end = size;

    struct {
        size_t model, palette;
    } layout;

    SCENE_STORAGE_RESERVE(layout, end, model, mat4f_t, skin->skin.joint_count);
    SCENE_STORAGE_RESERVE(layout, end, palette, mat4f_t, skin->skin.joint_count);
    void* storage = memory_alloc(end, MEMORY_FAST);
    if (storage == NULL) {
        return false;
    }
    if (scene->light_transfer != NULL && !scene_skin_light_allocate(skin)) {
        memory_free(storage);
        return false;
    }
    if (skin->light_mode == SKIN_LIGHT_TABLE && !light_table_join(scene)) {
        memory_free(skin->light_storage);
        skin->light_storage = NULL;
        memory_free(storage);
        return false;
    }
    skin->storage = storage;
    (void)r3d_skin_output_init(&skin->bind, storage, size, &skin->output);
    skin->model = (mat4f_t*)(void*)((uint8_t*)storage + layout.model);
    skin->palette = (mat4f_t*)(void*)((uint8_t*)storage + layout.palette);
    const scene_animator_t* animator = &scene->animators[entity];
    anim_pose_model(&animator->skeleton, &animator->layer.shown, skin->model);
    r3d_skin_palette(&skin->skin, skin->model, skin->bind.position_scale, skin->palette);
    r3d_skin_clusters(&skin->skin, &skin->bind, skin->palette, 0, skin->bind.cluster_count, &skin->output);
    r3d_skin_refit_nodes(&skin->output);
    if (skin->light_storage != NULL) {
        skin->output.mesh.colors = skin->colors;
    }
    return true;
}

void
scene_skin_give(scene_t* scene, scene_skinned_renderer_t* skin) {
    scene_skin_light_table_release(scene, skin);
    memory_free(skin->light_storage);
    memory_free(skin->storage);
    skin->light_storage = NULL;
    skin->storage = NULL;
    skin->colors = NULL;
    skin->normal_palette = NULL;
    skin->model = NULL;
    skin->palette = NULL;
    skin->output = (r3d_skin_output_t){0};
}

void
scene_skin_show(scene_t* scene, scene_row_t entity) {
    const scene_skinned_renderer_t* skin = &scene->skinned_renderers[entity];
    scene_renderer_t* r = &scene->renderers[scene_renderer_index(scene, entity)];
    r->mesh = skin->storage != NULL ? skin->output.mesh : skin->bind;
    scene_renderer_bound(r);
    scene->flags[entity] |= SCENE_FLAG_SKIN_DIRTY;
    scene->active_renderers_stale = true;
}

static void
starve(scene_t* scene, scene_row_t entity, scene_skinned_renderer_t* skin) {
    skin->starved = true;
    scene->starved_count++;
    scene->root->failure = scene_failure_make(SCENE_ERR_MEMORY, ASSET_OK, scene->asset.names[entity].name);
    scene->active_renderers_stale = true;
    (void)fprintf(stderr, "scene: no memory to skin %s; it is not drawn\n", scene->asset.names[entity].name);
}

void
scene_skins_settle(scene_t* scene) {
    if (!scene->skins_pending) {
        return;
    }
    scene->skins_pending = false;
    for (int i = 0; i < scene->staged_count; i++) {
        const scene_row_t e = scene->staged[i];
        scene_skinned_renderer_t* skin = &scene->skinned_renderers[e];
        if (!skin->bound || skin->storage != NULL || skin->starved || !(scene->flags[e] & SCENE_FLAG_ACTIVE)) {
            continue;
        }
        if (scene_skin_take(scene, e, skin)) {
            scene_skin_show(scene, e);
        } else {
            starve(scene, e, skin);
        }
    }
}

void
scene_skin_deactivate(scene_t* scene, scene_row_t entity) {
    scene_skinned_renderer_t* skin = &scene->skinned_renderers[entity];
    if (skin->starved) {
        skin->starved = false;
        scene->starved_count--;
    }
    if (skin->storage != NULL) {
        scene_skin_give(scene, skin);
        scene_skin_show(scene, entity);
    }
}

bool
skin_set_light_mode(entity_t entity, skin_light_mode_t mode) {
    scene_row_t row;
    scene_t* scene = scene_entity_row(entity, &row);
    if (scene == NULL) {
        return false;
    }
    if (mode < SKIN_LIGHT_DIRECT || mode > SKIN_LIGHT_TABLE) {
        return false;
    }
    return scene_skin_light_apply_mode(scene, row, &scene->skinned_renderers[row], mode);
}

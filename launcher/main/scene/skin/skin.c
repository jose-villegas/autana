/* skin: each lit skin's colour and normal-palette block, and how it is lit. */
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
    void* storage = scene_allocate(end, MEMORY_INTERNAL);
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

bool
scene_skin_light_apply_mode(scene_t* scene, scene_row_t entity, scene_skinned_renderer_t* skin,
                            skin_light_mode_t mode) {
    if (skin->internal_storage == NULL) {
        return false;
    }
    if (skin->light_mode == mode) {
        return true;
    }
    if (mode == SKIN_LIGHT_TABLE && scene->light_table == NULL) {
        r3d_skin_light_table_t* table = scene_allocate(sizeof *table, MEMORY_INTERNAL);
        if (table == NULL) {
            return false;
        }
        scene->light_table = table;
        scene->light_table_dirty = true;
    }
    if (skin->light_mode == SKIN_LIGHT_TABLE) {
        scene_skin_light_table_release(scene, entity);
    }
    if (mode == SKIN_LIGHT_TABLE) {
        scene->light_table_users++;
    }
    skin->light_mode = mode;
    scene->flags[entity] |= SCENE_FLAG_SKIN_DIRTY;
    return true;
}

void
scene_skin_light_table_release(scene_t* scene, scene_row_t entity) {
    if (scene->skinned_renderers[entity].light_mode != SKIN_LIGHT_TABLE) {
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

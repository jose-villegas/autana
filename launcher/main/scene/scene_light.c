/* Scene lights own shared transfer tables and each lit skin's color and normal-palette block. */
#include <math.h>
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

r3d_skin_light_transfer_t*
scene_light_transfer_take(const scene_t* scene) {
    r3d_skin_light_transfer_t* transfer = scene_allocate(sizeof *transfer, MEMORY_INTERNAL);
    if (transfer != NULL) {
        r3d_skin_light_transfer_build(scene->tonemap_white, transfer);
    }
    return transfer;
}

bool
scene_lights_allocate(scene_t* scene) {
    bool has_skin = false;
    for (int r = 0; r < scene->renderer_count; r++) {
        const scene_skinned_renderer_t* skin = &scene->skinned_renderers[scene->renderers[r].entity];
        has_skin |= skin->internal_storage != NULL;
        if (skin->internal_storage != NULL && skin->bind.colors == NULL) {
            (void)fprintf(stderr, "scene: %s\n", SCENE_LIGHT_FAILURE_ALBEDO);
            return false;
        }
    }
    if (!has_skin) {
        return true;
    }
    r3d_skin_light_transfer_t* transfer = scene_light_transfer_take(scene);
    if (transfer == NULL) {
        return false;
    }
    bool allocated = true;
    for (int r = 0; r < scene->renderer_count; r++) {
        scene_skinned_renderer_t* skin = &scene->skinned_renderers[scene->renderers[r].entity];
        if (skin->internal_storage != NULL && !scene_skin_light_allocate(skin)) {
            allocated = false;
            break;
        }
    }
    if (!allocated) {
        for (int r = 0; r < scene->renderer_count; r++) {
            scene_skinned_renderer_t* skin = &scene->skinned_renderers[scene->renderers[r].entity];
            memory_free(skin->light_storage);
            skin->light_storage = NULL;
            skin->colors = NULL;
            skin->normal_palette = NULL;
        }
        memory_free(transfer);
        return false;
    }
    scene->light_transfer = transfer;
    return true;
}

static scene_light_t*
light_attach(scene_t* scene, scene_entity_t entity) {
    /* Its direction is its own rotation, so a light is a plain root. */
    if (scene == NULL || entity >= scene->asset.entity_count || !scene_entity_is_plain_root(scene, entity)) {
        return NULL;
    }
    scene_light_t* light = &scene->lights[entity];
    if (light->kind != SCENE_LIGHT_NONE) {
        return light;
    }
    if (scene->light_count == R3D_SKIN_LIGHT_MAX) {
        return NULL;
    }
    if (scene->light_transfer == NULL && !scene_lights_allocate(scene)) {
        return NULL;
    }
    *light = (scene_light_t){SCENE_LIGHT_DIRECTIONAL, SCENE_LIGHT_COLOR_DEFAULT, SCENE_LIGHT_INTENSITY_DEFAULT};
    scene->light_count++;
    for (int r = 0; r < scene->renderer_count; r++) {
        scene_skinned_renderer_t* skin = &scene->skinned_renderers[scene->renderers[r].entity];
        if (skin->internal_storage != NULL) {
            skin->output.mesh.colors = skin->colors;
            scene->renderers[r].mesh.colors = skin->colors;
        }
    }
    return light;
}

bool
scene_light_color_fits(vec3f_t linear_rgb) {
    if (!isfinite(linear_rgb.x) || !isfinite(linear_rgb.y) || !isfinite(linear_rgb.z) || linear_rgb.x < 0.0F
        || linear_rgb.x > 1.0F || linear_rgb.y < 0.0F || linear_rgb.y > 1.0F || linear_rgb.z < 0.0F
        || linear_rgb.z > 1.0F) {
        return false;
    }
    return true;
}

bool
scene_light_set_color(scene_t* scene, scene_entity_t entity, vec3f_t linear_rgb) {
    if (!scene_light_color_fits(linear_rgb)) {
        return false;
    }
    scene_light_t* light = light_attach(scene, entity);
    if (light == NULL) {
        return false;
    }
    light->color = linear_rgb;
    scene->flags[entity] |= SCENE_FLAG_SKIN_DIRTY;
    return true;
}

bool
scene_light_set_intensity(scene_t* scene, scene_entity_t entity, float intensity) {
    if (!isfinite(intensity) || intensity < 0.0F) {
        return false;
    }
    scene_light_t* light = light_attach(scene, entity);
    if (light == NULL) {
        return false;
    }
    light->intensity = intensity;
    scene->flags[entity] |= SCENE_FLAG_SKIN_DIRTY;
    return true;
}

vec3f_t
scene_light_color(const scene_t* scene, scene_entity_t entity) {
    return scene == NULL || entity >= scene->asset.entity_count || scene->lights[entity].kind == SCENE_LIGHT_NONE
               ? (vec3f_t){0}
               : scene->lights[entity].color;
}

float
scene_light_intensity(const scene_t* scene, scene_entity_t entity) {
    return scene == NULL || entity >= scene->asset.entity_count || scene->lights[entity].kind == SCENE_LIGHT_NONE
               ? 0.0F
               : scene->lights[entity].intensity;
}

void
scene_skin_light_prepare(scene_t* scene, scene_entity_t entity) {
    if (scene->light_transfer == NULL) {
        return;
    }
    scene_skinned_renderer_t* skin = &scene->skinned_renderers[entity];
    skin->lighting_valid = r3d_skin_normal_palette(
        skin->palette, skin->skin.joint_count, transformf_compute_matrix(&scene->locals[entity]), skin->normal_palette);
    if (!skin->lighting_valid) {
        memset(skin->colors, 0, (size_t)skin->bind.vertex_count * R3D_SKIN_LIGHT_CHANNELS);
        (void)fprintf(stderr, "scene: %s: %s\n", SCENE_LIGHT_FAILURE_NORMAL, scene->asset.names[entity].name);
    }
}

void
scene_lights_prepare(scene_t* scene) {
    if (scene->light_transfer == NULL) {
        return;
    }
    r3d_skin_lights_t world = {.ambient = vec3f_scale(scene->ambient_color, scene->ambient_intensity),
                               .transfer = scene->light_transfer};
    for (int l = 0; l < scene->asset.entity_count; l++) {
        const scene_light_t* light = &scene->lights[l];
        if (light->kind == SCENE_LIGHT_NONE || !(scene->flags[l] & SCENE_FLAG_ACTIVE)) {
            continue;
        }
        const int at = world.count++;
        world.direction[at] = quatf_rotate(scene->locals[l].rotation, SCENE_LIGHT_AXIS);
        world.color[at] = vec3f_scale(light->color, light->intensity);
    }
    world.table = scene->light_table;
    if (scene->light_table != NULL && scene->light_table_dirty) {
        world.table_scale = r3d_skin_light_table_build(&world, scene->light_table);
        scene->light_table_dirty = false;
    } else {
        world.table_scale = scene->world_lights.table_scale;
    }
    scene->world_lights = world;
}

bool
scene_skin_light_set_mode(scene_t* scene, scene_entity_t entity, scene_skin_light_mode_t mode) {
    if (scene == NULL || entity >= scene->asset.entity_count || mode < SCENE_SKIN_LIGHT_DIRECT
        || mode > SCENE_SKIN_LIGHT_TABLE) {
        return false;
    }
    return scene_skin_light_apply_mode(scene, entity, &scene->skinned_renderers[entity], mode);
}

bool
scene_skin_light_apply_mode(scene_t* scene, scene_entity_t entity, scene_skinned_renderer_t* skin,
                            scene_skin_light_mode_t mode) {
    if (skin->internal_storage == NULL) {
        return false;
    }
    if (skin->light_mode == mode) {
        return true;
    }
    if (mode == SCENE_SKIN_LIGHT_TABLE && scene->light_table == NULL) {
        r3d_skin_light_table_t* table = scene_allocate(sizeof *table, MEMORY_INTERNAL);
        if (table == NULL) {
            return false;
        }
        scene->light_table = table;
        scene->light_table_dirty = true;
    }
    if (skin->light_mode == SCENE_SKIN_LIGHT_TABLE) {
        scene_skin_light_table_release(scene, entity);
    }
    if (mode == SCENE_SKIN_LIGHT_TABLE) {
        scene->light_table_users++;
    }
    skin->light_mode = mode;
    scene->flags[entity] |= SCENE_FLAG_SKIN_DIRTY;
    return true;
}

void
scene_skin_light_table_release(scene_t* scene, scene_entity_t entity) {
    if (scene->skinned_renderers[entity].light_mode != SCENE_SKIN_LIGHT_TABLE) {
        return;
    }
    scene->light_table_users--;
    if (scene->light_table_users == 0) {
        memory_free(scene->light_table);
        scene->light_table = NULL;
        scene->world_lights.table = NULL;
    }
}

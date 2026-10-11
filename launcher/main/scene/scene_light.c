/* Scene lights own shared transfer tables and each lit skin's temporary storage. */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "core/memory.h"
#include "scene/scene_internal.h"

#if !defined(ESP_PLATFORM) || CONFIG_LAUNCHER_DEVELOPMENT
int scene_fail_light_allocation_after = -1;
#endif

static void*
light_allocate(size_t bytes) {
#if !defined(ESP_PLATFORM) || CONFIG_LAUNCHER_DEVELOPMENT
    if (scene_fail_next_allocation || scene_fail_light_allocation_after == 0) {
        scene_fail_next_allocation = false;
        scene_fail_light_allocation_after = -1;
        return NULL;
    }
    if (scene_fail_light_allocation_after > 0) {
        scene_fail_light_allocation_after--;
    }
#endif
    return memory_alloc(bytes, MEMORY_INTERNAL);
}

bool
scene_skin_light_allocate(scene_skinned_renderer_t* skin) {
    size_t end = 0;

    struct {
        size_t colors, normals;
    } layout;

    SCENE_STORAGE_RESERVE(layout, end, colors, uint8_t, skin->bind.vertex_count* R3D_SKIN_LIGHT_CHANNELS);
    SCENE_STORAGE_RESERVE(layout, end, normals, mat3f_t, skin->skin.joint_count);
    void* storage = light_allocate(end);
    if (storage == NULL) {
        return false;
    }
    memset(storage, 0, end);
    skin->light_storage = storage;
    skin->colors = (uint8_t(*)[R3D_SKIN_LIGHT_CHANNELS])(void*)((uint8_t*)storage + layout.colors);
    skin->normal_palette = (mat3f_t*)(void*)((uint8_t*)storage + layout.normals);
    return true;
}

static bool
light_storage_allocate(scene_t* scene) {
    for (int r = 0; r < scene->asset.renderer_count; r++) {
        const scene_skinned_renderer_t* skin = &scene->skinned_renderers[scene->renderers[r].entity];
        if (skin->internal_storage != NULL && skin->bind.colors == NULL) {
            (void)fprintf(stderr, "scene: %s\n", SCENE_LIGHT_FAILURE_ALBEDO);
            return false;
        }
    }
    r3d_skin_light_transfer_t* transfer = light_allocate(sizeof *transfer);
    if (transfer == NULL) {
        return false;
    }
    bool allocated = true;
    for (int r = 0; r < scene->asset.renderer_count; r++) {
        scene_skinned_renderer_t* skin = &scene->skinned_renderers[scene->renderers[r].entity];
        if (skin->internal_storage != NULL && !scene_skin_light_allocate(skin)) {
            allocated = false;
            break;
        }
    }
    if (!allocated) {
        for (int r = 0; r < scene->asset.renderer_count; r++) {
            scene_skinned_renderer_t* skin = &scene->skinned_renderers[scene->renderers[r].entity];
            memory_free(skin->light_storage);
            skin->light_storage = NULL;
            skin->colors = NULL;
            skin->normal_palette = NULL;
        }
        memory_free(transfer);
        return false;
    }
    r3d_skin_light_transfer_build(scene->tonemap_white, transfer);
    scene->light_transfer = transfer;
    return true;
}

static scene_light_t*
light_attach(scene_t* scene, scene_entity_t entity) {
    if (scene == NULL || entity >= scene->asset.entity_count) {
        return NULL;
    }
    scene_light_t* light = &scene->lights[entity];
    if (light->kind != SCENE_LIGHT_NONE) {
        return light;
    }
    if (scene->light_count == R3D_SKIN_LIGHT_MAX) {
        return NULL;
    }
    if (scene->light_count == 0 && !light_storage_allocate(scene)) {
        return NULL;
    }
    *light = (scene_light_t){SCENE_LIGHT_DIRECTIONAL, SCENE_LIGHT_COLOR_DEFAULT, SCENE_LIGHT_INTENSITY_DEFAULT};
    scene->light_count++;
    for (int r = 0; r < scene->asset.renderer_count; r++) {
        scene_skinned_renderer_t* skin = &scene->skinned_renderers[scene->renderers[r].entity];
        if (skin->internal_storage != NULL) {
            skin->output.mesh.colors = skin->colors;
            scene->renderers[r].mesh.colors = skin->colors;
        }
    }
    return light;
}

bool
scene_light_set_color(scene_t* scene, scene_entity_t entity, vec3f_t linear_rgb) {
    if (!isfinite(linear_rgb.x) || !isfinite(linear_rgb.y) || !isfinite(linear_rgb.z) || linear_rgb.x < 0.0F
        || linear_rgb.x > 1.0F || linear_rgb.y < 0.0F || linear_rgb.y > 1.0F || linear_rgb.z < 0.0F
        || linear_rgb.z > 1.0F) {
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
    if (scene->light_count == 0) {
        return;
    }
    scene_skinned_renderer_t* skin = &scene->skinned_renderers[entity];
    skin->lighting_valid = r3d_skin_normal_palette(
        skin->palette, skin->skin.joint_count, transformf_compute_matrix(&scene->locals[entity]), skin->normal_palette);
    if (!skin->lighting_valid) {
        memset(skin->colors, 0, (size_t)skin->bind.vertex_count * R3D_SKIN_LIGHT_CHANNELS);
        (void)fprintf(stderr, "scene: %s: %s\n", SCENE_LIGHT_FAILURE_NORMAL, scene->asset.names[entity]);
    }
}

void
scene_lights_prepare(scene_t* scene) {
    if (scene->light_count == 0) {
        return;
    }
    r3d_skin_lights_t world = {.ambient = vec3f_scale(scene->ambient_color, scene->ambient_intensity),
                               .transfer = scene->light_transfer};
    for (int l = 0; l < scene->asset.entity_count; l++) {
        const scene_light_t* light = &scene->lights[l];
        if (light->kind == SCENE_LIGHT_NONE || !(scene->flags[l] & SCENE_FLAG_ENABLED)) {
            continue;
        }
        const int at = world.count++;
        world.direction[at] = quatf_rotate(scene->locals[l].rotation, SCENE_LIGHT_AXIS);
        world.color[at] = vec3f_scale(light->color, light->intensity);
    }
    scene->world_lights = world;
}

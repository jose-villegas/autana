/* light: directional lights and the scene's shared light transfer table. */
#include "scene/light/light.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include "core/memory.h"
#include "scene/scene_internal.h"

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
        has_skin |= skin->bound;
        if (skin->bound && skin->bind.colors == NULL) {
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
        if (skin->storage != NULL && !scene_skin_light_allocate(skin)) {
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
light_attach(scene_t* scene, scene_row_t entity) {
    /* Its direction is its own rotation under the scene root, so a light has no other parent. */
    if (scene_row_parent(scene, entity) != SCENE_ROW_NONE) {
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
    scene_stage_add(scene, entity);
    for (int r = 0; r < scene->renderer_count; r++) {
        scene_skinned_renderer_t* skin = &scene->skinned_renderers[scene->renderers[r].entity];
        if (skin->storage != NULL) {
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
light_set_color(entity_t entity, vec3f_t linear_rgb) {
    scene_row_t row;
    scene_t* scene = scene_entity_row(entity, &row);
    if (scene == NULL) {
        return false;
    }
    if (!scene_light_color_fits(linear_rgb)) {
        return false;
    }
    scene_light_t* light = light_attach(scene, row);
    if (light == NULL) {
        return false;
    }
    light->color = linear_rgb;
    scene->flags[row] |= SCENE_FLAG_SKIN_DIRTY;
    return true;
}

bool
light_set_intensity(entity_t entity, float intensity) {
    scene_row_t row;
    scene_t* scene = scene_entity_row(entity, &row);
    if (scene == NULL) {
        return false;
    }
    if (!isfinite(intensity) || intensity < 0.0F) {
        return false;
    }
    scene_light_t* light = light_attach(scene, row);
    if (light == NULL) {
        return false;
    }
    light->intensity = intensity;
    scene->flags[row] |= SCENE_FLAG_SKIN_DIRTY;
    return true;
}

vec3f_t
light_color(entity_t entity) {
    scene_row_t row;
    const scene_t* scene = scene_entity_row(entity, &row);
    if (scene == NULL) {
        return vec3f_zero();
    }
    return scene->lights[row].kind == SCENE_LIGHT_NONE ? vec3f_zero() : scene->lights[row].color;
}

float
light_intensity(entity_t entity) {
    scene_row_t row;
    const scene_t* scene = scene_entity_row(entity, &row);
    if (scene == NULL) {
        return 0.0F;
    }
    return scene->lights[row].kind == SCENE_LIGHT_NONE ? 0.0F : scene->lights[row].intensity;
}

void
scene_lights_prepare(scene_t* scene) {
    if (scene->light_transfer == NULL) {
        return;
    }
    r3d_skin_lights_t world = {.ambient = vec3f_scale(scene->ambient_color, scene->ambient_intensity),
                               .transfer = scene->light_transfer};
    for (int i = 0; i < scene->staged_count; i++) {
        const scene_row_t l = scene->staged[i];
        const scene_light_t* light = &scene->lights[l];
        if (light->kind == SCENE_LIGHT_NONE || !(scene->flags[l] & SCENE_FLAG_ACTIVE)) {
            continue;
        }
        const int at = world.count++;
        world.direction[at] = quatf_rotate(scene_row_world(scene->root, l).rotation, SCENE_LIGHT_AXIS);
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

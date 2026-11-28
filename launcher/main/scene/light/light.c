/* light: directional lights and the scene's shared light transfer table. */
#include "scene/light/light.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include "core/memory.h"
#include "scene/scene_internal.h"

/* Luminance of a linear RGB light as BT.709 weighs it. */
#define LIGHT_LUMA_BT709 ((vec3f_t){0.2126F, 0.7152F, 0.0722F})

r3d_skin_light_transfer_t*
scene_light_transfer_take(const scene_t* scene) {
    r3d_skin_light_transfer_t* transfer = scene_allocate(sizeof *transfer, MEMORY_INTERNAL);
    if (transfer != NULL) {
        r3d_skin_light_transfer_build(scene->tonemap_white, transfer);
    }
    return transfer;
}

static void
show_lit_skins(scene_t* scene) {
    for (int r = 0; r < scene->renderer_count; r++) {
        const scene_row_t e = scene->renderers[r].entity;
        if (scene->skinned_renderers[e].storage != NULL) {
            scene_skin_show(scene, e);
        }
    }
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
    show_lit_skins(scene);
    return true;
}

static scene_light_t*
light_attach(entity_t entity) {
    scene_row_t row;
    scene_t* scene = scene_entity_row(entity, &row);
    if (scene == NULL) {
        return NULL;
    }
    /* Its direction is its own rotation under the scene root, so a light has no other parent. */
    if (scene_row_parent(scene, row) != SCENE_ROW_NONE) {
        return NULL;
    }
    scene_light_t* light = &scene->lights[row];
    if (light->kind == SCENE_LIGHT_NONE) {
        if (scene->light_count == R3D_SKIN_LIGHT_MAX) {
            return NULL;
        }
        if (scene->light_transfer == NULL && !scene_lights_allocate(scene)) {
            return NULL;
        }
        *light = (scene_light_t){.kind = SCENE_LIGHT_DIRECTIONAL,
                                 .mode = LIGHT_MODE_REALTIME,
                                 .color = SCENE_LIGHT_COLOR_DEFAULT,
                                 .intensity = SCENE_LIGHT_INTENSITY_DEFAULT};
        scene->light_count++;
        scene_stage_add(scene, row);
    }
    scene->flags[row] |= SCENE_FLAG_SKIN_DIRTY;
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
    if (!scene_light_color_fits(linear_rgb)) {
        return false;
    }
    scene_light_t* light = light_attach(entity);
    if (light == NULL) {
        return false;
    }
    light->color = linear_rgb;
    return true;
}

bool
light_set_intensity(entity_t entity, float intensity) {
    if (!isfinite(intensity) || intensity < 0.0F) {
        return false;
    }
    scene_light_t* light = light_attach(entity);
    if (light == NULL) {
        return false;
    }
    light->intensity = intensity;
    return true;
}

bool
light_set_mode(entity_t entity, light_mode_t mode) {
    if (mode != LIGHT_MODE_REALTIME && mode != LIGHT_MODE_BAKED) {
        return false;
    }
    scene_light_t* light = light_attach(entity);
    if (light == NULL) {
        return false;
    }
    light->mode = mode;
    return true;
}

light_mode_t
light_mode(entity_t entity) {
    scene_row_t row;
    const scene_t* scene = scene_entity_row(entity, &row);
    return scene == NULL || scene->lights[row].kind == SCENE_LIGHT_NONE ? LIGHT_MODE_REALTIME : scene->lights[row].mode;
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

static bool
light_world(const scene_t* scene, scene_row_t row, vec3f_t* direction, vec3f_t* radiance) {
    const scene_light_t* light = &scene->lights[row];
    if (light->kind == SCENE_LIGHT_NONE || !(scene->flags[row] & SCENE_FLAG_ACTIVE)) {
        return false;
    }
    *direction = quatf_rotate(scene_row_world(scene->root, row).rotation, SCENE_LIGHT_AXIS);
    *radiance = vec3f_scale(light->color, light->intensity);
    return true;
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
        /* A baked light's share is already in the probes the skins sample. */
        if (scene->probes != NULL && scene->lights[l].mode == LIGHT_MODE_BAKED) {
            continue;
        }
        const int at = world.count;
        world.count += light_world(scene, l, &world.direction[at], &world.color[at]);
    }
    (void)r3d_skin_lights_tune(&world);
    world.table = scene->light_table;
    if (scene->light_table != NULL && scene->light_table_dirty) {
        world.table_scale = r3d_skin_light_table_build(&world, scene->light_table);
        scene->light_table_dirty = false;
    } else {
        world.table_scale = scene->world_lights.table_scale;
    }
    scene->world_lights = world;
}

void
scene_set_probes(entity_t root, const sh_probes_t* probes) {
    scene_row_t row;
    scene_root_t* record = scene_entity_lookup(root, &row);
    if (record == NULL || row != SCENE_ROW_ROOT || record->scene == NULL) {
        return;
    }
    scene_t* scene = record->scene;
    if (probes != NULL && scene->light_transfer == NULL && !scene_lights_allocate(scene)) {
        return;
    }
    scene->probes = probes;
    scene->light_table_dirty = true;
    const bool unlit = !scene_is_lit(scene);
    if (unlit) {
        memory_free(scene->light_transfer);
        scene->light_transfer = NULL;
        scene->world_lights.transfer = NULL;
    }
    for (int r = 0; r < scene->renderer_count; r++) {
        const scene_row_t e = scene->renderers[r].entity;
        if (unlit) {
            scene_skin_deactivate(scene, e);
        }
        if (scene->skinned_renderers[e].bound) {
            scene->flags[e] |= SCENE_FLAG_SKIN_DIRTY;
        }
    }
    scene->skins_pending = true;
}

const sh_probes_t*
scene_probes(entity_t root) {
    scene_row_t row;
    scene_root_t* record = scene_entity_lookup(root, &row);
    return record != NULL && row == SCENE_ROW_ROOT && record->scene != NULL ? record->scene->probes : NULL;
}

vec3f_t
scene_light_brightest_direction(const scene_t* scene) {
    vec3f_t brightest = vec3f_zero();
    float luminance = 0;
    for (int i = 0; i < scene->staged_count; i++) {
        vec3f_t direction;
        vec3f_t radiance;
        if (!light_world(scene, scene->staged[i], &direction, &radiance)) {
            continue;
        }
        const float value = vec3f_dot(radiance, LIGHT_LUMA_BT709);
        if (value > luminance) {
            luminance = value;
            brightest = direction;
        }
    }
    return brightest;
}

vec3f_t
scene_contact_light(const scene_t* scene, vec3f_t world, scene_t* const* drawn_scenes, int drawn_scene_count) {
    /* Draw order lets a receiver scene light movers from separately loaded scenes. */
    for (int k = 0; k < drawn_scene_count; k++) {
        const vec3f_t direction = scene_light_brightest_direction(drawn_scenes[k]);
        if (vec3f_dot(direction, direction) > 0) {
            return direction;
        }
    }
    if (scene->probes != NULL) {
        const sh1_rgb_t radiance = sh_probes_sample(scene->probes, world);
        const vec3f_t direction = sh1_rgb_dominant_direction(&radiance, LIGHT_LUMA_BT709);
        if (vec3f_dot(direction, direction) > 0) {
            return direction;
        }
    }
    return vec3f_unit_y();
}

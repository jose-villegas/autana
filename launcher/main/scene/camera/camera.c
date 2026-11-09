/* camera: the camera component; scene_draw.c draws through the active one. */
#include "scene/camera/camera.h"

#include <math.h>

#include "scene/scene_internal.h"

int
scene_camera_index(const scene_t* scene, scene_row_t entity) {
    for (int i = 0; i < scene->asset.camera_count; i++) {
        if (scene->cameras[i].entity == entity) {
            return i;
        }
    }
    return -1;
}

static scene_camera_t*
camera_component(entity_t camera, scene_t** scene) {
    scene_row_t row;
    *scene = scene_entity_row(camera, &row);
    const int index = *scene == NULL ? -1 : scene_camera_index(*scene, row);
    return index < 0 ? NULL : &(*scene)->cameras[index];
}

static lens_t*
camera_of(entity_t camera) {
    scene_t* scene;
    scene_camera_t* found = camera_component(camera, &scene);
    return found == NULL ? NULL : &found->lens;
}

entity_t
camera_first(entity_t within) {
    for (entity_t e = entity_next(within, ENTITY_NONE); !handle_is_none(e); e = entity_next(within, e)) {
        if (camera_of(e) != NULL) {
            return e;
        }
    }
    return ENTITY_NONE;
}

const lens_t*
camera_lens(entity_t camera) {
    return camera_of(camera);
}

bool
camera_activate(entity_t camera) {
    scene_t* scene;
    const scene_camera_t* found = camera_component(camera, &scene);
    if (found == NULL) {
        return false;
    }
    scene_draw_activate(scene, (int)(found - scene->cameras));
    return true;
}

bool
camera_set_present(entity_t camera, camera_present_t present) {
    lens_t* found = camera_of(camera);
    if (found == NULL || present < CAMERA_PRESENT_NEAREST || present > CAMERA_PRESENT_DOUBLE) {
        return false;
    }
    found->present = present;
    return true;
}

bool
camera_set_shift(entity_t camera, vec2f_t shift) {
    lens_t* found = camera_of(camera);
    if (found == NULL || !isfinite(shift.x) || !isfinite(shift.y)) {
        return false;
    }
    found->shift = shift;
    return true;
}

bool
camera_set_small_fill(entity_t camera, small_fill_t fill) {
    lens_t* found = camera_of(camera);
    if (found == NULL || fill.ceiling > SMALL_FILL_CEILING_MAX || fill.style >= SMALL_FILL_STYLE_COUNT) {
        return false;
    }
    found->small_fill = fill;
    return true;
}

bool
camera_set_skip_hidden(entity_t camera, bool skip) {
    lens_t* found = camera_of(camera);
    if (found == NULL) {
        return false;
    }
    found->skip_hidden = skip;
    return true;
}

bool
camera_set_curves(entity_t camera, bool curves) {
    lens_t* found = camera_of(camera);
    if (found == NULL) {
        return false;
    }
    found->curves = curves;
    return true;
}

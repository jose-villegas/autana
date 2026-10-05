#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "scene/scene_internal.h"

/* The header, written by tools/r3d/scene_asset.py: a u16 version and three
 * u16 counts, then the u32 offset of each section. */
enum {
    HEADER_SIZE = 24,
    AT_ENTITIES = 2,
    AT_RENDERERS = 4,
    AT_CAMERAS = 6,
    AT_NAMES = 8,
    AT_TRANSFORMS = 12,
    AT_RENDERER_ROWS = 16,
    AT_CAMERA_ROWS = 20,
};

_Static_assert(sizeof(scene_transform_t) == 12 * sizeof(float), "a transform is a 3x3 and a position");
_Static_assert(sizeof(scene_asset_renderer_t) == 36 && offsetof(scene_asset_renderer_t, mesh) == 4,
               "a renderer row is not the layout scene_asset.py writes");
_Static_assert(sizeof(scene_asset_camera_t) == 80 && offsetof(scene_asset_camera_t, clip) == 16
                   && offsetof(scene_asset_camera_t, node) == 48,
               "a camera row is not the layout scene_asset.py writes");

static uint16_t
half(const uint8_t* at) {
    return (uint16_t)(at[0] | (at[1] << 8));
}

static uint32_t
word(const uint8_t* at) {
    return (uint32_t)at[0] | ((uint32_t)at[1] << 8) | ((uint32_t)at[2] << 16) | ((uint32_t)at[3] << 24);
}

/* The section at the header's `field`: `count` rows of `row` bytes, after the
 * header, inside the entry and 4-aligned; NULL when it is not. */
static const void*
section(asset_view_t entry, int field, uint32_t count, size_t row) {
    const uint32_t offset = word(entry.data + field);
    if (offset % 4U != 0 || offset < HEADER_SIZE || (uint64_t)offset + ((uint64_t)count * row) > entry.size) {
        return NULL;
    }
    return entry.data + offset;
}

/* Some text, then nothing but NULs to the end of the field. */
static bool
text_fits(const char* field, bool may_be_empty) {
    const char* end = memchr(field, '\0', ASSET_NAME_MAX);
    if (end == NULL || (end == field && !may_be_empty)) {
        return false;
    }
    for (const char* at = end; at < field + ASSET_NAME_MAX; at++) {
        if (*at != '\0') {
            return false;
        }
    }
    return true;
}

static bool
positive(float value) {
    return isfinite(value) && value > 0.0F;
}

static bool
camera_fits(const scene_asset_t* scene, const scene_asset_camera_t* c) {
    return c->entity < scene->entity_count && c->pad == 0 && positive(c->half_fov_short_tan) && positive(c->near_z)
           && c->clear_rgb <= 0xFFFFFFU && text_fits(c->clip, true) && text_fits(c->node, true)
           && (c->clip[0] == '\0') == (c->node[0] == '\0');
}

static asset_status_t
check_rows(const scene_asset_t* scene) {
    for (int i = 0; i < scene->entity_count; i++) {
        if (!text_fits(scene->names[i], false)) {
            return ASSET_ERR_FORMAT;
        }
    }
    for (int i = 0; i < scene->renderer_count; i++) {
        const scene_asset_renderer_t* r = &scene->renderers[i];
        if (r->entity >= scene->entity_count || r->pad != 0 || !text_fits(r->mesh, false)) {
            return ASSET_ERR_FORMAT;
        }
    }
    for (int i = 0; i < scene->camera_count; i++) {
        if (!camera_fits(scene, &scene->cameras[i])) {
            return ASSET_ERR_FORMAT;
        }
    }
    return ASSET_OK;
}

asset_status_t
scene_asset_open(asset_view_t entry, scene_asset_t* out) {
    *out = (scene_asset_t){0};
    /* The rows are read in place, so the entry itself must be 4-aligned, as a pack's entries are. */
    if (entry.size < HEADER_SIZE || (uintptr_t)entry.data % 4U != 0) {
        return ASSET_ERR_BOUNDS;
    }
    if (half(entry.data) != SCENE_ASSET_VERSION) {
        return ASSET_ERR_VERSION;
    }
    scene_asset_t scene = {
        .entity_count = half(entry.data + AT_ENTITIES),
        .renderer_count = half(entry.data + AT_RENDERERS),
        .camera_count = half(entry.data + AT_CAMERAS),
    };
    scene.names = section(entry, AT_NAMES, scene.entity_count, ASSET_NAME_MAX);
    scene.transforms = section(entry, AT_TRANSFORMS, scene.entity_count, sizeof(scene_transform_t));
    scene.renderers = section(entry, AT_RENDERER_ROWS, scene.renderer_count, sizeof(scene_asset_renderer_t));
    scene.cameras = section(entry, AT_CAMERA_ROWS, scene.camera_count, sizeof(scene_asset_camera_t));
    if (scene.names == NULL || scene.transforms == NULL || scene.renderers == NULL || scene.cameras == NULL) {
        return ASSET_ERR_BOUNDS;
    }
    const asset_status_t status = check_rows(&scene);
    if (status == ASSET_OK) {
        *out = scene;
    }
    return status;
}

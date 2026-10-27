/* scene_asset: validates mapped SCNE rows before scene instantiation. */
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "asset/asset_bytes.h"
#include "asset/asset_entry.h"
#include "scene/scene_internal.h"

enum {
    AT_VERSION = 0,
    ENTRY_ALIGNMENT = 4,
    AT_HEADER_PAD = SCENE_ASSET_AT_COUNTS + (SCENE_ASSET_PART_COUNT * sizeof(uint16_t)) /* zero up to the offsets */
};

_Static_assert(sizeof(scene_asset_name_t) == SCENE_ASSET_NAME_SIZE
                   && offsetof(scene_asset_name_t, hash) == SCENE_ASSET_NAME_AT_HASH
                   && offsetof(scene_asset_name_t, name) == SCENE_ASSET_NAME_AT_TEXT,
               "SCNE names row layout");
_Static_assert(sizeof(scene_asset_transform_t) == 48 && sizeof(scene_asset_renderer_t) == 36
                   && sizeof(scene_asset_camera_t) == 16 && sizeof(scene_asset_light_t) == 20
                   && sizeof(scene_asset_skinned_renderer_t) == 100 && sizeof(scene_asset_animator_t) == 12
                   && sizeof(scene_asset_settings_t) == 20,
               "SCNE mapped row layouts");
_Static_assert(SCENE_ASSET_AT_COUNTS == sizeof(uint16_t)
                   && SCENE_ASSET_AT_OFFSETS
                          == (AT_HEADER_PAD + ENTRY_ALIGNMENT - 1) / ENTRY_ALIGNMENT * ENTRY_ALIGNMENT
                   && SCENE_ASSET_HEADER_SIZE == SCENE_ASSET_AT_OFFSETS + (SCENE_ASSET_PART_COUNT * sizeof(uint32_t)),
               "SCNE header matches its part count");
_Static_assert(sizeof(((scene_asset_t*)0)->names) == sizeof(void*), "mapped view pointer width");

static uint16_t
count(asset_view_t entry, int part) {
    return asset_read_u16(entry.data + SCENE_ASSET_AT_COUNTS + (part * sizeof(uint16_t)));
}

static const void*
section(asset_view_t entry, int part, size_t row) {
    const uint32_t offset = asset_read_u32(entry.data + SCENE_ASSET_AT_OFFSETS + (part * sizeof(uint32_t)));
    if (offset % ENTRY_ALIGNMENT != 0 || offset < SCENE_ASSET_HEADER_SIZE
        || (uint64_t)offset + ((uint64_t)count(entry, part) * row) > entry.size) {
        return NULL;
    }
    return entry.data + offset;
}

static bool
text_fits(const void* row) {
    const char* field = row;
    const char* end = memchr(field, '\0', ASSET_NAME_MAX);
    if (end == NULL || end == field) {
        return false;
    }
    for (const char* at = end; at < field + ASSET_NAME_MAX; at++) {
        if (*at != '\0') {
            return false;
        }
    }
    return true;
}

/* The stored hash is the name's, so finds compare it alone. */
static bool
name_fits(const void* row) {
    const scene_asset_name_t* name = row;
    return text_fits(name->name) && name->hash != NAME_HASH_NONE && name->hash == name_hash(name->name);
}

static bool
nonnegative(float value) {
    return isfinite(value) && value >= 0.0F;
}

/* The writer pads the parents part, two bytes a row, to 4; the pad is zero. */
static bool
parents_pad_zero(asset_view_t entry, const scene_asset_t* scene) {
    const uint32_t end = (uint32_t)((const uint8_t*)(scene->parents + scene->entity_count) - entry.data);
    const uint32_t padded = end + ((ENTRY_ALIGNMENT - (end % ENTRY_ALIGNMENT)) % ENTRY_ALIGNMENT);
    return asset_entry_zero(entry, end, padded < entry.size ? padded : entry.size);
}

/* A root whose transform does not mirror (scene_entity_is_plain_root). */
static bool
plain_root(const scene_asset_t* scene, uint16_t entity) {
    return scene->parents[entity] == SCENE_ASSET_PARENT_ROOT
           && mat3f_determinant(scene->transforms[entity].linear) > 0.0F;
}

/* A parent comes before its child, so one forward pass sees every parent first. */
static bool
parent_fits(const scene_asset_t* scene, const void* row) {
    const uint16_t* parent = row;
    return *parent == SCENE_ASSET_PARENT_ROOT || *parent < parent - scene->parents;
}

static bool
renderer_fits(const scene_asset_t* scene, const void* row) {
    const scene_asset_renderer_t* r = row;
    return r->entity < scene->entity_count && text_fits(r->mesh);
}

static bool
camera_fits(const scene_asset_t* scene, const void* row) {
    const scene_asset_camera_t* c = row;
    return c->entity < scene->entity_count && plain_root(scene, c->entity) && c->pad == 0
           && isfinite(c->half_fov_short_tan) && c->half_fov_short_tan > 0.0F && isfinite(c->near_z) && c->near_z > 0.0F
           && c->clear_rgb <= 0xFFFFFFU;
}

static bool
light_fits(const scene_asset_t* scene, const void* row) {
    const scene_asset_light_t* light = row;
    return light->entity < scene->entity_count && plain_root(scene, light->entity) && light->pad == 0
           && light->kind == SCENE_LIGHT_KIND_DIRECTIONAL && scene_light_color_fits(light->color)
           && nonnegative(light->intensity);
}

static bool
skin_fits(const scene_asset_t* scene, const void* row) {
    const scene_asset_skinned_renderer_t* skin = row;
    return skin->entity < scene->entity_count && plain_root(scene, skin->entity) && skin->pad == 0
           && text_fits(skin->mesh) && text_fits(skin->skin) && text_fits(skin->skeleton);
}

static bool
animator_fits(const scene_asset_t* scene, const void* row) {
    const scene_asset_animator_t* a = row;
    return a->entity < scene->entity_count && a->pad == 0 && (a->wrap == ANIM_CLAMP || a->wrap == ANIM_LOOP)
           && (uint64_t)a->first_clip + a->clip_count <= scene->clip_count
           && (a->default_clip == SCENE_ASSET_DEFAULT_CLIP_NONE || a->default_clip < a->clip_count);
}

static bool
settings_fit(const scene_asset_t* scene, const void* row) {
    (void)scene;
    const scene_asset_settings_t* settings = row;
    return scene_light_color_fits(settings->ambient_color) && nonnegative(settings->ambient_intensity)
           && nonnegative(settings->tonemap_white);
}

typedef struct {
    size_t stride, view, count;
} part_layout_t;

#define PART_LAYOUT(member, count_member)                                                                              \
    {sizeof *((scene_asset_t*)0)->member, offsetof(scene_asset_t, member), offsetof(scene_asset_t, count_member)}

static const part_layout_t LAYOUT[SCENE_ASSET_PART_COUNT] = {
    PART_LAYOUT(names, entity_count),
    PART_LAYOUT(transforms, entity_count),
    PART_LAYOUT(renderers, renderer_count),
    PART_LAYOUT(cameras, camera_count),
    PART_LAYOUT(lights, light_count),
    PART_LAYOUT(skinned_renderers, skinned_renderer_count),
    PART_LAYOUT(animators, animator_count),
    PART_LAYOUT(clips, clip_count),
    {sizeof(scene_asset_settings_t), offsetof(scene_asset_t, settings), SIZE_MAX},
    PART_LAYOUT(parents, entity_count)};

static bool
row_fits(const scene_asset_t* scene, int part, const void* row) {
    switch (part) {
        case SCENE_ASSET_NAMES: return name_fits(row);
        case SCENE_ASSET_CLIPS: return text_fits(row);
        case SCENE_ASSET_TRANSFORMS: return true;
        case SCENE_ASSET_RENDERERS: return renderer_fits(scene, row);
        case SCENE_ASSET_CAMERAS: return camera_fits(scene, row);
        case SCENE_ASSET_LIGHTS: return light_fits(scene, row);
        case SCENE_ASSET_SKINS: return skin_fits(scene, row);
        case SCENE_ASSET_ANIMATORS: return animator_fits(scene, row);
        case SCENE_ASSET_SETTINGS: return settings_fit(scene, row);
        case SCENE_ASSET_PARENTS: return parent_fits(scene, row);
        default: return false;
    }
}

static bool
entities_unique(const uint8_t* rows, size_t stride, int count) {
    for (int i = 0; i < count; i++) {
        const uint16_t entity = asset_read_u16(rows + (i * stride));
        for (int j = 0; j < i; j++) {
            if (entity == asset_read_u16(rows + (j * stride))) {
                return false;
            }
        }
    }
    return true;
}

static asset_status_t
check_rows(const scene_asset_t* scene) {
    for (int part = 0; part < SCENE_ASSET_PART_COUNT; part++) {
        const part_layout_t* layout = &LAYOUT[part];
        uint16_t count = 1;
        if (layout->count != SIZE_MAX) {
            memcpy(&count, (const uint8_t*)scene + layout->count, sizeof count);
        }
        const uint8_t* rows;
        memcpy(&rows, (const uint8_t*)scene + layout->view, sizeof rows);
        if ((part == SCENE_ASSET_LIGHTS || part == SCENE_ASSET_SKINS || part == SCENE_ASSET_ANIMATORS)
            && !entities_unique(rows, layout->stride, count)) {
            return ASSET_ERR_FORMAT;
        }
        for (int i = 0; i < count; i++) {
            if (!row_fits(scene, part, rows + (i * layout->stride))) {
                return ASSET_ERR_FORMAT;
            }
        }
    }
    return ASSET_OK;
}

asset_status_t
scene_asset_open(asset_view_t entry, scene_asset_t* out) {
    *out = (scene_asset_t){0};
    /* A v1 entry may be shorter than the v2 header; report its version before its bounds. */
    if (entry.size >= sizeof(uint16_t) && asset_read_u16(entry.data) != SCENE_ASSET_VERSION) {
        return ASSET_ERR_VERSION;
    }
    const asset_status_t status =
        asset_entry_validate(entry, SCENE_ASSET_VERSION, AT_VERSION, SCENE_ASSET_HEADER_SIZE, ENTRY_ALIGNMENT);
    if (status != ASSET_OK) {
        return status;
    }
    const uint16_t entities = count(entry, SCENE_ASSET_NAMES);
    if (count(entry, SCENE_ASSET_TRANSFORMS) != entities || count(entry, SCENE_ASSET_PARENTS) != entities
        || entities == SCENE_ENTITY_NONE || count(entry, SCENE_ASSET_SETTINGS) != 1
        || !asset_entry_zero(entry, AT_HEADER_PAD, SCENE_ASSET_AT_OFFSETS)) {
        return ASSET_ERR_FORMAT;
    }
    scene_asset_t scene = {0};
    for (int part = 0; part < SCENE_ASSET_PART_COUNT; part++) {
        const part_layout_t* layout = &LAYOUT[part];
        const void* rows = section(entry, part, layout->stride);
        if (rows == NULL) {
            return ASSET_ERR_BOUNDS;
        }
        memcpy((uint8_t*)&scene + layout->view, &rows, sizeof rows);
        if (layout->count != SIZE_MAX) {
            const uint16_t rows_count = count(entry, part);
            memcpy((uint8_t*)&scene + layout->count, &rows_count, sizeof rows_count);
        }
    }
    if (!parents_pad_zero(entry, &scene)) {
        return ASSET_ERR_FORMAT;
    }
    const asset_status_t checked = check_rows(&scene);
    if (checked == ASSET_OK) {
        *out = scene;
    }
    return checked;
}

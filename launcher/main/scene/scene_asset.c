/* scene_asset: validates mapped SCNE rows before scene instantiation. */
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "asset/asset_bytes.h"
#include "asset/asset_entry.h"
#include "collections/bits.h"
#include "core/memory.h"
#include "scene/scene_asset.h"
#include "scene/scene_internal.h"

enum {
    AT_VERSION = 0,
    ENTRY_ALIGNMENT = 4,
    AT_HEADER_PAD = SCENE_ASSET_AT_COUNTS + (SCENE_ASSET_PART_COUNT * sizeof(uint16_t)) /* zero up to the offsets */
};

_Static_assert(sizeof(scene_asset_name_t) == SCENE_ASSET_NAME_SIZE
                   && SCNE_FIELD_AT(scene_asset_name_t, hash, NAME_AT_HASH)
                   && SCNE_FIELD_AT(scene_asset_name_t, name, NAME_AT_TEXT),
               "SCNE names row layout");
_Static_assert(sizeof(scene_asset_transform_t) == 48 && sizeof(scene_asset_renderer_t) == 36
                   && sizeof(scene_asset_camera_t) == 16 && sizeof(scene_asset_light_t) == 20
                   && sizeof(scene_asset_skinned_renderer_t) == 100 && sizeof(scene_asset_animator_t) == 12
                   && sizeof(scene_asset_settings_t) == 20 && sizeof(scene_asset_batch_t) == 12
                   && sizeof(r3d_copy_chunk_t) == 16 && sizeof(r3d_copy_place_t) == 8 && sizeof(r3d_copy_turn_t) == 8,
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

static bool
root_row_fits(const scene_asset_t* scene, uint16_t entity, uint16_t pad) {
    return pad == 0 && entity < scene->entity_count && scene->parents[entity] == SCENE_ASSET_PARENT_ROOT;
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
    return root_row_fits(scene, c->entity, c->pad) && lens_value_fits(c->half_fov_short_tan)
           && lens_value_fits(c->near_z) && c->clear_rgb <= 0xFFFFFFU;
}

static bool
light_fits(const scene_asset_t* scene, const void* row) {
    const scene_asset_light_t* light = row;
    return root_row_fits(scene, light->entity, light->pad) && light->kind == SCENE_LIGHT_KIND_DIRECTIONAL
           && scene_light_color_fits(light->color) && nonnegative(light->intensity);
}

static bool
skin_fits(const scene_asset_t* scene, const void* row) {
    const scene_asset_skinned_renderer_t* skin = row;
    return root_row_fits(scene, skin->entity, skin->pad) && text_fits(skin->mesh) && text_fits(skin->skin)
           && text_fits(skin->skeleton);
}

static bool
animator_fits(const scene_asset_t* scene, const void* row) {
    const scene_asset_animator_t* a = row;
    return a->entity < scene->entity_count && a->pad == 0 && (a->wrap == ANIM_CLAMP || a->wrap == ANIM_LOOP)
           && (uint64_t)a->first_clip + a->clip_count <= scene->clip_count
           && (a->default_clip == SCENE_ASSET_DEFAULT_CLIP_NONE || a->default_clip < a->clip_count);
}

static bool
timeline_fits(const scene_asset_timeline_t* timeline) {
    static const uint8_t ZERO[sizeof timeline->pad] = {0};
    return text_fits(timeline->clip) && (timeline->wrap == ANIM_CLAMP || timeline->wrap == ANIM_LOOP)
           && memcmp(timeline->pad, ZERO, sizeof ZERO) == 0;
}

static bool
vector_finite(vec3f_t value) {
    return isfinite(value.x) && isfinite(value.y) && isfinite(value.z);
}

scene_row_t
scene_asset_find_row(const scene_asset_t* scene, name_hash_t target) {
    for (scene_row_t i = 0; i < scene->entity_count; i++) {
        if (scene->names[i].hash == target) {
            return i;
        }
    }
    return SCENE_ROW_NONE;
}

static bool
source_range_fits(const scene_asset_t* scene, uint16_t first, uint8_t count) {
    return count > 0 && count <= SCENE_CONSTRAINT_SOURCES_MAX && first + count <= scene->source_count;
}

static bool
position_constraint_fits(const scene_asset_t* scene, const scene_asset_position_constraint_t* position) {
    return position->entity < scene->entity_count && position->pad == 0
           && position->binding_mode <= SCENE_ASSET_BIND_LOCK_TO_TARGET_WITH_WORLD_UP
           && source_range_fits(scene, position->source_first, position->source_count)
           && scene_constraint_weight_fits(position->weight) && vector_finite(position->translation_offset);
}

static bool
direction_fits(vec3f_t direction) {
    return vector_finite(direction) && (direction.x != 0.0F || direction.y != 0.0F || direction.z != 0.0F);
}

static bool
aim_constraint_fits(const scene_asset_t* scene, const scene_asset_aim_constraint_t* aim) {
    const bool object_up = aim->world_up_type == SCENE_ASSET_WORLD_UP_OBJECT_UP
                           || aim->world_up_type == SCENE_ASSET_WORLD_UP_OBJECT_ROTATION_UP;
    const quatf_t rotation = aim->rotation_offset;
    return aim->entity < scene->entity_count && aim->world_up_type <= SCENE_ASSET_WORLD_UP_NONE
           && (object_up ? aim->world_up_object < scene->entity_count
                         : aim->world_up_object == SCENE_ASSET_WORLD_UP_OBJECT_NONE)
           && source_range_fits(scene, aim->source_first, aim->source_count)
           && scene_constraint_weight_fits(aim->weight) && direction_fits(aim->aim_vector)
           && direction_fits(aim->up_vector) && direction_fits(aim->world_up_vector) && isfinite(rotation.x)
           && isfinite(rotation.y) && isfinite(rotation.z) && isfinite(rotation.w)
           && fabsf(rotation.x * rotation.x + rotation.y * rotation.y + rotation.z * rotation.z
                    + rotation.w * rotation.w - 1.0F)
                  <= SCENE_ASSET_AIM_ROTATION_TOLERANCE;
}

static bool
constraint_source_fits(const scene_asset_t* scene, const scene_asset_constraint_source_t* source) {
    return scene_constraint_weight_fits(source->weight)
           && scene_asset_find_row(scene, source->target) != SCENE_ROW_NONE;
}

static bool
hinge_fits(const scene_asset_t* scene, const scene_asset_hinge_t* h) {
    return h->entity < scene->entity_count && h->pad == 0 && isfinite(h->min) && isfinite(h->max) && isfinite(h->angle)
           && h->min <= h->max && fabsf(vec3f_dot(h->axis, h->axis) - 1.0F) <= SCENE_ASSET_HINGE_AXIS_TOLERANCE;
}

static bool
settings_fit(const scene_asset_t* scene, const void* row) {
    (void)scene;
    const scene_asset_settings_t* settings = row;
    return scene_light_color_fits(settings->ambient_color) && nonnegative(settings->ambient_intensity)
           && nonnegative(settings->tonemap_white);
}

static bool
batch_fits(const scene_asset_t* scene, const void* row) {
    const scene_asset_batch_t* b = row;
    return b->renderer < scene->renderer_count && b->copy_count > 0;
}

static bool
chunk_fits(const void* row) {
    const r3d_copy_chunk_t* c = row;
    return isfinite(c->centre.x) && isfinite(c->centre.y) && isfinite(c->centre.z) && nonnegative(c->reach);
}

static bool
turn_fits(const void* row) {
    const r3d_copy_turn_t* t = row;
    return t->largest < 4U;
}

/* Batches take the copies and chunks in turn, every one, and a renderer at most once. */
static bool
batches_cover(const scene_asset_t* scene) {
    uint32_t copies = 0;
    uint32_t chunks = 0;
    for (int i = 0; i < scene->batch_count; i++) {
        const scene_asset_batch_t* b = &scene->batches[i];
        if (b->first_copy != copies || b->first_chunk != chunks) {
            return false;
        }
        copies += b->copy_count;
        chunks += (uint32_t)r3d_batch_chunks(b->copy_count);
    }
    return copies == scene->copy_count && chunks == scene->chunk_count;
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
    PART_LAYOUT(parents, entity_count),
    PART_LAYOUT(batches, batch_count),
    PART_LAYOUT(chunks, chunk_count),
    PART_LAYOUT(places, copy_count),
    {sizeof(r3d_copy_turn_t), offsetof(scene_asset_t, turns), offsetof(scene_asset_t, copy_count)},
    PART_LAYOUT(timeline, timeline_count),
    PART_LAYOUT(position_constraints, position_constraint_count),
    PART_LAYOUT(aim_constraints, aim_constraint_count),
    PART_LAYOUT(sources, source_count),
    PART_LAYOUT(hinges, hinge_count),
};

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
        case SCENE_ASSET_BATCHES: return batch_fits(scene, row);
        case SCENE_ASSET_CHUNKS: return chunk_fits(row);
        case SCENE_ASSET_PLACES: return true;
        case SCENE_ASSET_TURNS: return turn_fits(row);
        case SCENE_ASSET_TIMELINE: return timeline_fits(row);
        case SCENE_ASSET_POSITION_CONSTRAINTS: return position_constraint_fits(scene, row);
        case SCENE_ASSET_AIM_CONSTRAINTS: return aim_constraint_fits(scene, row);
        case SCENE_ASSET_CONSTRAINT_SOURCES: return constraint_source_fits(scene, row);
        case SCENE_ASSET_HINGES: return hinge_fits(scene, row);
        default: return false;
    }
}

/* Row validation has already bounded each index. */
static asset_status_t
indices_unique(const uint8_t* rows, size_t stride, int count, uint16_t limit) {
    if (count < 2) {
        return ASSET_OK;
    }
    if (count > limit) {
        return ASSET_ERR_FORMAT;
    }
    const size_t bytes = BITS8_BYTES(limit);
    uint8_t* seen = memory_alloc(bytes, MEMORY_APP);
    if (seen == NULL) {
        return ASSET_ERR_FULL;
    }
    memset(seen, 0, bytes);
    asset_status_t status = ASSET_OK;
    for (int i = 0; i < count; i++) {
        const uint16_t index = asset_read_u16(rows + (i * stride));
        if (bits8_get(seen, index)) {
            status = ASSET_ERR_FORMAT;
            break;
        }
        bits8_set(seen, index);
    }
    memory_free(seen);
    return status;
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
        for (int i = 0; i < count; i++) {
            if (!row_fits(scene, part, rows + (i * layout->stride))) {
                return ASSET_ERR_FORMAT;
            }
        }
        if (part == SCENE_ASSET_LIGHTS || part == SCENE_ASSET_SKINS || part == SCENE_ASSET_ANIMATORS
            || part == SCENE_ASSET_POSITION_CONSTRAINTS || part == SCENE_ASSET_AIM_CONSTRAINTS
            || part == SCENE_ASSET_HINGES || part == SCENE_ASSET_BATCHES) {
            const uint16_t limit = part == SCENE_ASSET_BATCHES ? scene->renderer_count : scene->entity_count;
            const asset_status_t status = indices_unique(rows, layout->stride, count, limit);
            if (status != ASSET_OK) {
                return status;
            }
        }
    }
    return ASSET_OK;
}

asset_status_t
scene_asset_open(asset_view_t entry, scene_asset_t* out) {
    *out = (scene_asset_t){0};
    /* Report a version mismatch even when the entry is shorter than this header. */
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
        || count(entry, SCENE_ASSET_TURNS) != count(entry, SCENE_ASSET_PLACES) || entities == SCENE_ROW_NONE
        || count(entry, SCENE_ASSET_SETTINGS) != 1 || count(entry, SCENE_ASSET_TIMELINE) > 1
        || !asset_entry_zero(entry, AT_HEADER_PAD, SCENE_ASSET_AT_OFFSETS)) {
        return ASSET_ERR_FORMAT;
    }
    scene_asset_t scene = {0};

    struct {
        uint32_t start;
        uint64_t end;
    } ranges[SCENE_ASSET_PART_COUNT];

    int occupied = 0;
    for (int part = 0; part < SCENE_ASSET_PART_COUNT; part++) {
        const part_layout_t* layout = &LAYOUT[part];
        const void* rows = section(entry, part, layout->stride);
        if (rows == NULL) {
            return ASSET_ERR_BOUNDS;
        }
        const uint16_t rows_count = count(entry, part);
        if (rows_count != 0) {
            const uint32_t start = (uint32_t)((const uint8_t*)rows - entry.data);
            const uint64_t bytes = (uint64_t)rows_count * layout->stride;
            const uint64_t end = start + ((bytes + ENTRY_ALIGNMENT - 1U) / ENTRY_ALIGNMENT * ENTRY_ALIGNMENT);
            for (int i = 0; i < occupied; i++) {
                if (start < ranges[i].end && ranges[i].start < end) {
                    return ASSET_ERR_FORMAT;
                }
            }
            ranges[occupied].start = start;
            ranges[occupied++].end = end;
        }
        memcpy((uint8_t*)&scene + layout->view, &rows, sizeof rows);
        if (layout->count != SIZE_MAX) {
            memcpy((uint8_t*)&scene + layout->count, &rows_count, sizeof rows_count);
        }
    }
    /* The scene numbers its renderers, plain then skinned, in a uint16_t. */
    if (!parents_pad_zero(entry, &scene) || scene.renderer_count + scene.skinned_renderer_count > UINT16_MAX) {
        return ASSET_ERR_FORMAT;
    }
    asset_status_t checked = check_rows(&scene);
    if (checked == ASSET_OK && !batches_cover(&scene)) {
        checked = ASSET_ERR_FORMAT;
    }
    if (checked == ASSET_OK) {
        *out = scene;
    }
    return checked;
}

r3d_batch_t
scene_asset_batch(const scene_asset_t* asset, int row, const r3d_lit_mesh_t* mesh, const r3d_copy_chunk_t* chunks) {
    const scene_asset_batch_t* batch = &asset->batches[row];
    return (r3d_batch_t){.chunks = chunks + batch->first_chunk,
                         .places = asset->places + batch->first_copy,
                         .turns = asset->turns + batch->first_copy,
                         .colour_stride = (uint32_t)(mesh->colors != NULL ? mesh->vertex_count : mesh->triangle_count),
                         .copy_radius = r3d_batch_copy_radius(mesh),
                         .copy_count = batch->copy_count,
                         .chunk_count = (uint16_t)r3d_batch_chunks(batch->copy_count)};
}

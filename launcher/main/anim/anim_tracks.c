#include "anim/anim_tracks.h"

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

_Static_assert(ANIM_TRACKS_AT_PAD + ANIM_TRACKS_HEADER_PAD_SIZE == ANIM_TRACKS_HEADER_SIZE,
               "TRCK header fields fill the header");
_Static_assert(ANIM_TRACKS_ROW_PAD + ANIM_TRACKS_ROW_PAD_SIZE == ANIM_TRACKS_ROW_SIZE,
               "TRCK row fields and alignment fill the row");

static const uint8_t WIDTHS[] = {1, 2, 3, 4, 3};

static uint16_t
half(const uint8_t* at) {
    return (uint16_t)(at[0] | (at[1] << 8));
}

static uint32_t
word(const uint8_t* at) {
    return (uint32_t)at[0] | ((uint32_t)at[1] << 8) | ((uint32_t)at[2] << 16) | ((uint32_t)at[3] << 24);
}

static const uint8_t*
row_at(const anim_tracks_t* tracks, int index) {
    return tracks->base + ANIM_TRACKS_HEADER_SIZE + ((size_t)index * ANIM_TRACKS_ROW_SIZE);
}

static bool
floats_fit(const anim_tracks_t* tracks, uint32_t offset, uint32_t count) {
    const uint64_t strings_end =
        (uint64_t)word(tracks->base + ANIM_TRACKS_AT_STRINGS) + word(tracks->base + ANIM_TRACKS_AT_STRINGS_SIZE);
    return offset % ANIM_TRACKS_ALIGNMENT == 0 && offset >= strings_end
           && (uint64_t)offset + ((uint64_t)count * sizeof(float)) <= tracks->size;
}

static bool
string_fits(const anim_tracks_t* tracks, uint16_t offset) {
    const uint32_t size = word(tracks->base + ANIM_TRACKS_AT_STRINGS_SIZE);
    const uint8_t* strings = tracks->base + word(tracks->base + ANIM_TRACKS_AT_STRINGS);
    return offset < size && memchr(strings + offset, 0, size - offset) != NULL;
}

static asset_status_t
check_row(const anim_tracks_t* tracks, const uint8_t* row) {
    if (!string_fits(tracks, half(row + ANIM_TRACKS_ROW_PATH))
        || !string_fits(tracks, half(row + ANIM_TRACKS_ROW_FIELD))) {
        return ASSET_ERR_FORMAT;
    }
    const uint32_t keys = half(row + ANIM_TRACKS_ROW_KEYS);
    const uint8_t type = row[ANIM_TRACKS_ROW_TYPE];
    const uint8_t interp = row[ANIM_TRACKS_ROW_INTERP];
    const uint32_t component = word(row + ANIM_TRACKS_ROW_COMPONENT);
    if (keys == 0 || type > ANIM_VALUE_COLOUR || interp > ANIM_CUBIC
        || (component != ANIM_COMPONENT_TRANSFORM && component != ANIM_COMPONENT_CAMERA)) {
        return ASSET_ERR_FORMAT;
    }
    for (int i = ANIM_TRACKS_ROW_PAD; i < ANIM_TRACKS_ROW_SIZE; i++) {
        if (row[i] != 0) {
            return ASSET_ERR_FORMAT;
        }
    }
    const uint32_t values_count = keys * WIDTHS[type] * (interp == ANIM_CUBIC ? ANIM_TRACKS_CUBIC_RUNS : 1U);
    if (!floats_fit(tracks, word(row + ANIM_TRACKS_ROW_TIMES), keys)
        || !floats_fit(tracks, word(row + ANIM_TRACKS_ROW_VALUES), values_count)) {
        return ASSET_ERR_BOUNDS;
    }
    const float* times = (const float*)(const void*)(tracks->base + word(row + ANIM_TRACKS_ROW_TIMES));
    const float* values = (const float*)(const void*)(tracks->base + word(row + ANIM_TRACKS_ROW_VALUES));
    for (uint32_t i = 0; i < keys; i++) {
        if (!isfinite(times[i]) || (i > 0 && times[i] <= times[i - 1])) {
            return ASSET_ERR_FORMAT;
        }
    }
    for (uint32_t i = 0; i < values_count; i++) {
        if (!isfinite(values[i])) {
            return ASSET_ERR_FORMAT;
        }
    }
    return ASSET_OK;
}

asset_status_t
anim_tracks_open(asset_view_t entry, anim_tracks_t* out) {
    *out = (anim_tracks_t){0};
    if (entry.size < sizeof(uint16_t) || entry.data == NULL) {
        return ASSET_ERR_BOUNDS;
    }
    if (half(entry.data + ANIM_TRACKS_AT_VERSION) != ANIM_TRACKS_VERSION) {
        return ASSET_ERR_VERSION;
    }
    if (entry.size < ANIM_TRACKS_HEADER_SIZE || (uintptr_t)entry.data % ANIM_TRACKS_ALIGNMENT != 0) {
        return ASSET_ERR_BOUNDS;
    }
    const anim_tracks_t tracks = {
        .base = entry.data,
        .size = entry.size,
        .count = half(entry.data + ANIM_TRACKS_AT_COUNT),
        .root = entry.data[ANIM_TRACKS_AT_ROOT],
        .clip = {word(entry.data + ANIM_TRACKS_AT_DURATION)},
    };
    const uint32_t table_end = ANIM_TRACKS_HEADER_SIZE + ((uint32_t)tracks.count * ANIM_TRACKS_ROW_SIZE);
    const uint32_t strings = word(entry.data + ANIM_TRACKS_AT_STRINGS);
    const uint32_t strings_size = word(entry.data + ANIM_TRACKS_AT_STRINGS_SIZE);
    if (table_end > entry.size || strings < table_end || strings % ANIM_TRACKS_ALIGNMENT != 0
        || (uint64_t)strings + strings_size > entry.size) {
        return ASSET_ERR_BOUNDS;
    }
    if (tracks.root > ANIM_ROOT_SKELETON) {
        return ASSET_ERR_FORMAT;
    }
    for (int i = ANIM_TRACKS_AT_PAD; i < ANIM_TRACKS_HEADER_SIZE; i++) {
        if (entry.data[i] != 0) {
            return ASSET_ERR_FORMAT;
        }
    }
    for (int i = 0; i < tracks.count; i++) {
        const asset_status_t status = check_row(&tracks, row_at(&tracks, i));
        if (status != ASSET_OK) {
            return status;
        }
    }
    *out = tracks;
    return ASSET_OK;
}

asset_status_t
anim_tracks_from_pack(const asset_pack_t* pack, const char* id, anim_tracks_t* out) {
    asset_view_t entry;
    const asset_status_t status = asset_pack_find(pack, id, ANIM_TRACKS_ASSET, &entry);
    if (status != ASSET_OK) {
        *out = (anim_tracks_t){0};
        return status;
    }
    return anim_tracks_open(entry, out);
}

asset_status_t
anim_tracks_binding_at(const anim_tracks_t* tracks, int index, anim_binding_t* out) {
    *out = (anim_binding_t){0};
    if (index < 0 || index >= tracks->count) {
        return ASSET_ERR_NOT_FOUND;
    }
    const uint8_t* row = row_at(tracks, index);
    const char* strings = (const char*)tracks->base + word(tracks->base + ANIM_TRACKS_AT_STRINGS);
    const anim_value_t type = (anim_value_t)row[ANIM_TRACKS_ROW_TYPE];
    *out = (anim_binding_t){
        .path = strings + half(row + ANIM_TRACKS_ROW_PATH),
        .field = strings + half(row + ANIM_TRACKS_ROW_FIELD),
        .component = word(row + ANIM_TRACKS_ROW_COMPONENT),
        .type = type,
        .curve =
            {
                .times = (const float*)(const void*)(tracks->base + word(row + ANIM_TRACKS_ROW_TIMES)),
                .values = (const float*)(const void*)(tracks->base + word(row + ANIM_TRACKS_ROW_VALUES)),
                .count = half(row + ANIM_TRACKS_ROW_KEYS),
                .width = WIDTHS[type],
                .interp = row[ANIM_TRACKS_ROW_INTERP],
                .quaternion = type == ANIM_VALUE_QUAT,
            },
    };
    return ASSET_OK;
}

asset_status_t
anim_tracks_find(const anim_tracks_t* tracks, const char* path, uint32_t component, const char* field,
                 anim_track_t* out) {
    for (int i = 0; i < tracks->count; i++) {
        anim_binding_t binding;
        (void)anim_tracks_binding_at(tracks, i, &binding);
        if (binding.component == component && strcmp(binding.path, path) == 0 && strcmp(binding.field, field) == 0) {
            *out = binding.curve;
            return ASSET_OK;
        }
    }
    *out = (anim_track_t){0};
    return ASSET_ERR_NOT_FOUND;
}

static asset_status_t
find_part(const anim_tracks_t* tracks, const char* node, const char* field, anim_value_t type, anim_track_t* out) {
    const asset_status_t status = anim_tracks_find(tracks, node, ANIM_COMPONENT_TRANSFORM, field, out);
    if (status == ASSET_OK && (out->width != WIDTHS[type] || (out->quaternion != 0) != (type == ANIM_VALUE_QUAT))) {
        return ASSET_ERR_FORMAT;
    }
    return status;
}

asset_status_t
anim_tracks_find_node(const anim_tracks_t* tracks, const char* node, anim_node_tracks_t* out) {
    static const float AT_START[] = {0.0F};
    static const float UNSCALED[] = {1.0F, 1.0F, 1.0F};
    asset_status_t status = find_part(tracks, node, "position", ANIM_VALUE_VEC3, &out->translation);
    if (status == ASSET_OK) {
        status = find_part(tracks, node, "rotation", ANIM_VALUE_QUAT, &out->rotation);
    }
    if (status == ASSET_OK) {
        status = find_part(tracks, node, "scale", ANIM_VALUE_VEC3, &out->scale);
        if (status == ASSET_ERR_NOT_FOUND) {
            out->scale = (anim_track_t){AT_START, UNSCALED, 1, 3, ANIM_STEP, 0};
            status = ASSET_OK;
        }
    }
    return status;
}

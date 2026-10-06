#include "anim/anim_tracks.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* The entry's layout, written by tools/anim/tracks_asset.py. */
enum {
    HEADER_SIZE = 8,
    AT_COUNT = 2,
    AT_DURATION = 4,
    ROW_SIZE = 48,
    ROW_TIMES = 32,
    ROW_VALUES = 36,
    ROW_KEYS = 40,
    ROW_WIDTH = 42,
    ROW_INTERP = 43,
    ROW_QUATERNION = 44,
    ROW_PAD = 45,
    ROW_PAD_SIZE = 3,
};

_Static_assert(ROW_TIMES == ANIM_TRACK_NAME_MAX && ROW_PAD + ROW_PAD_SIZE == ROW_SIZE,
               "a track row is not the layout tracks_asset.py writes");

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
    return tracks->base + HEADER_SIZE + ((size_t)index * ROW_SIZE);
}

/* `count` floats at `offset`: after the table, inside the entry, 4-aligned. */
static bool
floats_fit(const anim_tracks_t* tracks, uint32_t offset, uint32_t count) {
    const uint32_t table_end = HEADER_SIZE + ((uint32_t)tracks->count * ROW_SIZE);
    return offset % 4U == 0 && offset >= table_end
           && (uint64_t)offset + ((uint64_t)count * sizeof(float)) <= tracks->size;
}

static asset_status_t
check_row(const anim_tracks_t* tracks, const uint8_t* row) {
    const uint32_t keys = half(row + ROW_KEYS);
    const uint32_t width = row[ROW_WIDTH];
    const uint8_t interp = row[ROW_INTERP];
    const uint8_t quaternion = row[ROW_QUATERNION];
    if (memchr(row, '\0', ANIM_TRACK_NAME_MAX) == NULL || keys == 0 || width == 0 || width > ANIM_WIDTH_MAX
        || interp > ANIM_CUBIC || quaternion > 1 || (quaternion != 0 && width != 4)
        || (row[ROW_PAD] | row[ROW_PAD + 1] | row[ROW_PAD + 2]) != 0) {
        return ASSET_ERR_FORMAT;
    }
    const uint32_t per_key = interp == ANIM_CUBIC ? 3U : 1U;
    if (!floats_fit(tracks, word(row + ROW_TIMES), keys)
        || !floats_fit(tracks, word(row + ROW_VALUES), per_key * keys * width)) {
        return ASSET_ERR_BOUNDS;
    }
    return ASSET_OK;
}

asset_status_t
anim_tracks_open(asset_view_t entry, anim_tracks_t* out) {
    *out = (anim_tracks_t){0};
    /* The floats are read in place, so the entry itself must be 4-aligned, as a pack's entries are. */
    if (entry.size < HEADER_SIZE || (uintptr_t)entry.data % 4U != 0) {
        return ASSET_ERR_BOUNDS;
    }
    if (half(entry.data) != ANIM_TRACKS_VERSION) {
        return ASSET_ERR_VERSION;
    }
    const anim_tracks_t tracks = {
        .base = entry.data,
        .size = entry.size,
        .count = half(entry.data + AT_COUNT),
        .clip = {word(entry.data + AT_DURATION)},
    };
    if (HEADER_SIZE + ((uint32_t)tracks.count * ROW_SIZE) > entry.size) {
        return ASSET_ERR_BOUNDS;
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
anim_tracks_at(const anim_tracks_t* tracks, int index, const char** name, anim_track_t* out) {
    if (index < 0 || index >= tracks->count) {
        *name = NULL;
        *out = (anim_track_t){0};
        return ASSET_ERR_NOT_FOUND;
    }
    const uint8_t* row = row_at(tracks, index);
    *name = (const char*)row;
    *out = (anim_track_t){
        .times = (const float*)(const void*)(tracks->base + word(row + ROW_TIMES)),
        .values = (const float*)(const void*)(tracks->base + word(row + ROW_VALUES)),
        .count = half(row + ROW_KEYS),
        .width = row[ROW_WIDTH],
        .interp = row[ROW_INTERP],
        .quaternion = row[ROW_QUATERNION],
    };
    return ASSET_OK;
}

asset_status_t
anim_tracks_find(const anim_tracks_t* tracks, const char* name, anim_track_t* out) {
    for (int i = 0; i < tracks->count; i++) {
        const char* row_name;
        (void)anim_tracks_at(tracks, i, &row_name, out);
        if (strncmp(row_name, name, ANIM_TRACK_NAME_MAX) == 0) {
            return ASSET_OK;
        }
    }
    *out = (anim_track_t){0};
    return ASSET_ERR_NOT_FOUND;
}

/* The track `<node>/<part>`, `width` wide and a quaternion exactly when 4. */
static asset_status_t
find_part(const anim_tracks_t* tracks, const char* node, const char* part, uint8_t width, anim_track_t* out) {
    char name[ANIM_TRACK_NAME_MAX];
    const int length = snprintf(name, sizeof name, "%s/%s", node, part);
    if (length < 0 || (size_t)length >= sizeof name) {
        *out = (anim_track_t){0};
        return ASSET_ERR_FORMAT;
    }
    const asset_status_t status = anim_tracks_find(tracks, name, out);
    if (status == ASSET_OK && (out->width != width || (out->quaternion != 0) != (width == 4))) {
        return ASSET_ERR_FORMAT;
    }
    return status;
}

asset_status_t
anim_tracks_find_node(const anim_tracks_t* tracks, const char* node, anim_node_tracks_t* out) {
    static const float AT_START[] = {0.0F};
    static const float UNSCALED[] = {1.0F, 1.0F, 1.0F};
    asset_status_t status = find_part(tracks, node, "translation", 3, &out->translation);
    if (status == ASSET_OK) {
        status = find_part(tracks, node, "rotation", 4, &out->rotation);
    }
    if (status == ASSET_OK) {
        status = find_part(tracks, node, "scale", 3, &out->scale);
        if (status == ASSET_ERR_NOT_FOUND) {
            out->scale = (anim_track_t){AT_START, UNSCALED, 1, 3, ANIM_STEP, 0};
            status = ASSET_OK;
        }
    }
    return status;
}

/*
 * anim_tracks: validated bindings and curves in a TRCK pack entry.
 * Views point into the entry, which must outlive them. No allocation.
 */
#pragma once

#include <stdint.h>
#include "anim/anim_track.h"
#include "asset/asset_pack.h"

#define ANIM_TRACKS_ASSET        ASSET_TYPE('T', 'R', 'C', 'K')
#define ANIM_TRACKS_VERSION      2U
#define ANIM_COMPONENT_TRANSFORM ASSET_TYPE('T', 'R', 'N', 'S')
#define ANIM_COMPONENT_CAMERA    ASSET_TYPE('C', 'A', 'M', 'R')

enum {
    ANIM_ROOT_SCENE,
    ANIM_ROOT_SKELETON,
};

enum {
    ANIM_TRACKS_HEADER_SIZE = 20,
    ANIM_TRACKS_AT_VERSION = 0,
    ANIM_TRACKS_AT_COUNT = 2,
    ANIM_TRACKS_AT_DURATION = 4,
    ANIM_TRACKS_AT_STRINGS = 8,
    ANIM_TRACKS_AT_STRINGS_SIZE = 12,
    ANIM_TRACKS_AT_ROOT = 16,
    ANIM_TRACKS_AT_PAD = 17,
    ANIM_TRACKS_HEADER_PAD_SIZE = 3,
    ANIM_TRACKS_ROW_SIZE = 24,
    ANIM_TRACKS_ROW_PATH = 0,
    ANIM_TRACKS_ROW_FIELD = 2,
    ANIM_TRACKS_ROW_COMPONENT = 4,
    ANIM_TRACKS_ROW_TIMES = 8,
    ANIM_TRACKS_ROW_VALUES = 12,
    ANIM_TRACKS_ROW_KEYS = 16,
    ANIM_TRACKS_ROW_TYPE = 18,
    ANIM_TRACKS_ROW_INTERP = 19,
    ANIM_TRACKS_ROW_PAD = 20,
    ANIM_TRACKS_ROW_PAD_SIZE = 4,
    ANIM_TRACKS_ALIGNMENT = 4,
    ANIM_TRACKS_CUBIC_RUNS = 3,
};

typedef struct {
    const uint8_t* base;
    uint32_t size;
    uint16_t count;
    uint8_t root;
    anim_clip_t clip;
} anim_tracks_t;

typedef struct {
    const char* path;
    const char* field;
    uint32_t component;
    anim_value_t type;
    anim_track_t curve;
} anim_binding_t;

asset_status_t anim_tracks_open(asset_view_t entry, anim_tracks_t* out);
asset_status_t anim_tracks_from_pack(const asset_pack_t* pack, const char* id, anim_tracks_t* out);
asset_status_t anim_tracks_binding_at(const anim_tracks_t* tracks, int index, anim_binding_t* out);
asset_status_t anim_tracks_find(const anim_tracks_t* tracks, const char* path, uint32_t component, const char* field,
                                anim_track_t* out);
/* Translation and rotation are required; absent scale is unit scale. */
asset_status_t anim_tracks_find_node(const anim_tracks_t* tracks, const char* node, anim_node_tracks_t* out);

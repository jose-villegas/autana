/*
 * anim_tracks: the bindings and curves of a TRCK pack entry, written by
 * launcher/tools/anim/tracks_asset.py; the layout is in docs/Animation-Tracks.md.
 * Views point into the entry, which must outlive them. No allocation.
 */
#pragma once

#include <stdint.h>
#include "anim/anim_track.h"
#include "anim/anim_tracks_format.h"
#include "asset/asset_pack.h"

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
/* position and rotation are required; absent scale is unit scale.
 * A wrong-type field returns ASSET_ERR_FORMAT. */
asset_status_t anim_tracks_find_node(const anim_tracks_t* tracks, const char* node, anim_node_tracks_t* out);

/*
 * anim_tracks: the tracks of one glTF animation as an asset-pack entry
 * (TRCK), written by launcher/tools/anim/tracks_asset.py.
 * Opening it checks every row once; a track found by name then points into
 * the entry, so nothing is copied. The layout is in docs/Animation-Tracks.md.
 *
 * No allocation and no ESP-IDF.
 */
#pragma once

#include <stdint.h>

#include "anim/anim_track.h"
#include "asset/asset_pack.h"

#define ANIM_TRACKS_ASSET   ASSET_TYPE('T', 'R', 'C', 'K')
#define ANIM_TRACKS_VERSION 1U
#define ANIM_TRACK_NAME_MAX 32U /* including the NUL */

/* An opened entry. Holds no copy: the pack must outlive it. */
typedef struct {
    const uint8_t* base;
    uint32_t size;
    uint16_t count;
    anim_clip_t clip;
} anim_tracks_t;

/* Checks the entry once, as docs/Animation-Tracks.md lists, and returns the
 * first failure: ASSET_ERR_VERSION, ASSET_ERR_BOUNDS or ASSET_ERR_FORMAT.
 * Else fills `out`. */
asset_status_t anim_tracks_open(asset_view_t entry, anim_tracks_t* out);

/* The TRCK entry `id` of `pack`, opened. */
asset_status_t anim_tracks_from_pack(const asset_pack_t* pack, const char* id, anim_tracks_t* out);

/* The track named `name` (its glTF binding, e.g. "camera/translation"),
 * pointing into the entry; ASSET_ERR_NOT_FOUND when the clip has none. */
asset_status_t anim_tracks_find(const anim_tracks_t* tracks, const char* name, anim_track_t* out);

/* The tracks of node `node` ("<node>/translation", "/rotation", "/scale"),
 * pointing into the entry. Translation and rotation must be there; a node the
 * clip does not scale keeps unit scale. ASSET_ERR_FORMAT when a track is not
 * the width it drives, or the rotation is not a quaternion. */
asset_status_t anim_tracks_find_node(const anim_tracks_t* tracks, const char* node, anim_node_tracks_t* out);

/* Track `index` of the entry's table and its name, both pointing into the
 * entry; ASSET_ERR_NOT_FOUND when `index` is outside [0, count). */
asset_status_t anim_tracks_at(const anim_tracks_t* tracks, int index, const char** name, anim_track_t* out);

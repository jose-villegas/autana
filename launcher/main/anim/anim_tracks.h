/*
 * anim_tracks: the tracks of one glTF animation as an asset-pack entry
 * (TRCK), baked from a NAME.anim.toml by launcher/tools/anim/tracks_asset.py.
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

/* Checks the version, the table and every track: a NUL-terminated name, a
 * width of 1 to ANIM_WIDTH_MAX, an anim_interp_t, a quaternion only of width
 * 4, and its times and values inside the entry, after the table and 4-byte
 * aligned. ASSET_ERR_VERSION, ASSET_ERR_BOUNDS or ASSET_ERR_FORMAT, else
 * fills `out`. */
asset_status_t anim_tracks_open(asset_view_t entry, anim_tracks_t* out);

/* The TRCK entry `id` of `pack`, opened. */
asset_status_t anim_tracks_from_pack(const asset_pack_t* pack, const char* id, anim_tracks_t* out);

/* The track named `name` (its glTF binding, e.g. "camera/translation"),
 * pointing into the entry; ASSET_ERR_NOT_FOUND when the clip has none. */
asset_status_t anim_tracks_find(const anim_tracks_t* tracks, const char* name, anim_track_t* out);

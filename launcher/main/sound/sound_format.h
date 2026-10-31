/*
 * sound_format: the SOND file layout, a sound as it is stored in an asset
 * pack. launcher/tools/audio/sound_asset.py writes it and sound.c reads it;
 * both take every value below from this header, and
 * docs/assets/formats/SOND.md draws it. sound.h includes it for the format
 * codes; code that plays sounds needs only sound.h.
 */
#pragma once

#include "asset/asset_pack.h"

/* The entry: its pack type tag and the layout version. */
#define SOUND_ASSET                ASSET_TYPE('S', 'O', 'N', 'D')
#define SOUND_VERSION              1U

/* The 32-byte little-endian header: its size, the data alignment, and where
 * each field starts. */
#define SOUND_HEADER_SIZE          32U
#define SOUND_ALIGNMENT            4U
#define SOUND_AT_VERSION           0U
#define SOUND_AT_FORMAT            2U
#define SOUND_AT_CHANNELS          3U
#define SOUND_AT_SAMPLE_RATE       4U
#define SOUND_AT_FRAMES            8U
#define SOUND_AT_LOOP_START        12U
#define SOUND_AT_LOOP_END          16U
#define SOUND_AT_BLOCK_FRAMES      20U
#define SOUND_AT_RESERVED16        22U
#define SOUND_AT_RESERVED32        24U
#define SOUND_AT_DATA              28U

/* What the header's fields may hold. */
#define SOUND_CHANNELS             1U
#define SOUND_PCM_BYTES            2U
#define SOUND_DEFAULT_BLOCK_FRAMES 505U

/* The format codes stored at SOUND_AT_FORMAT. Each code is defined once, here,
 * as its on-disk value, because the baker reads it with read_define() in
 * launcher/tools/build/c_header.py;
 * sound.h's sound_format_t is initialised from these. */
#define SOUND_FORMAT_PCM16         0U
#define SOUND_FORMAT_IMA_ADPCM     1U

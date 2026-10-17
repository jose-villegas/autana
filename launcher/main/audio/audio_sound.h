/*
 * audio_sound: a sound as an asset-pack entry (SOND), written by
 * launcher/tools/audio/sound_asset.py. Opening it checks the header, the loop
 * and every ADPCM block header; the samples then stay in the pack, so a mapped
 * pack plays with no copy and no RAM. A cursor reads them out as int16. The
 * layout is in docs/assets/README.md, "The sound entry".
 *
 * No allocation and no ESP-IDF.
 */
#pragma once

#include <stdint.h>
#include "asset/asset_pack.h"
#include "audio/audio_adpcm.h"

#define AUDIO_SOUND_ASSET                ASSET_TYPE('S', 'O', 'N', 'D')
#define AUDIO_SOUND_VERSION              1U
/* The stored rate every sound is baked at. */
#define AUDIO_SOUND_SAMPLE_RATE_HZ       22050U
#define AUDIO_SOUND_HEADER_SIZE          32U
#define AUDIO_SOUND_ALIGNMENT            4U
#define AUDIO_SOUND_CHANNELS             1U
#define AUDIO_SOUND_DEFAULT_BLOCK_FRAMES 505U
#define AUDIO_SOUND_PCM_BYTES            2U
#define AUDIO_SOUND_AT_VERSION           0U
#define AUDIO_SOUND_AT_FORMAT            2U
#define AUDIO_SOUND_AT_CHANNELS          3U
#define AUDIO_SOUND_AT_SAMPLE_RATE       4U
#define AUDIO_SOUND_AT_FRAMES            8U
#define AUDIO_SOUND_AT_LOOP_START        12U
#define AUDIO_SOUND_AT_LOOP_END          16U
#define AUDIO_SOUND_AT_BLOCK_FRAMES      20U
#define AUDIO_SOUND_AT_RESERVED16        22U
#define AUDIO_SOUND_AT_RESERVED32        24U
#define AUDIO_SOUND_AT_DATA              28U

#define AUDIO_SOUND_FORMAT_PCM16         0U
#define AUDIO_SOUND_FORMAT_IMA_ADPCM     1U

typedef enum {
    AUDIO_SOUND_PCM16 = AUDIO_SOUND_FORMAT_PCM16,
    AUDIO_SOUND_IMA_ADPCM = AUDIO_SOUND_FORMAT_IMA_ADPCM
} audio_sound_format_t;

typedef struct {
    const uint8_t* data;         /* PCM16 LE samples or the first ADPCM block header; pack-owned. */
    uint32_t frames;             /* Stored mono sample count. */
    uint32_t loop_start;         /* First frame of the loop. */
    uint32_t loop_end;           /* Exclusive loop end; zero means one-shot. */
    uint16_t block_frames;       /* Samples per ADPCM block; zero for PCM16. */
    audio_sound_format_t format; /* Sample encoding at data. */
} audio_sound_t;

typedef struct {
    const audio_sound_t* sound; /* Validated view; the view and its pack must outlive this cursor. */
    uint32_t position;          /* Next stored frame to read, for bounding reads at loop_end. */
    audio_adpcm_t adpcm;        /* Current block's codes and IMA state; unused for PCM16. */
} audio_sound_cursor_t;

/* ADPCM seeks must be block-aligned, except the end position; failures zero the cursor. */
asset_status_t audio_sound_seek(audio_sound_cursor_t* c, const audio_sound_t* sound, uint32_t frame);
/* Copies at most frames int16 samples, stopping at the sound's end. Loops require a seek. */
uint32_t audio_sound_read(audio_sound_cursor_t* c, int16_t* out, uint32_t frames);

asset_status_t audio_sound_open(const asset_pack_t* pack, const char* name, audio_sound_t* out);

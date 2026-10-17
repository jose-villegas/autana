/* A validated SOND view into a pack that must outlive the view; no allocation. */
#pragma once

#include <stdint.h>
#include "asset/asset_pack.h"

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
    const uint8_t* data;
    uint32_t frames, loop_start, loop_end;
    uint16_t block_frames;
    audio_sound_format_t format;
} audio_sound_t;

asset_status_t audio_sound_open(const asset_pack_t* pack, const char* name, audio_sound_t* out);

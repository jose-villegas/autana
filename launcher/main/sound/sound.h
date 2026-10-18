/*
 * sound: a sound as an asset-pack entry (SOND), written by
 * launcher/tools/audio/sound_asset.py. Opening it checks the header, the loop
 * and every ADPCM block header; the samples then stay in the pack, so a mapped
 * pack plays with no copy and no RAM. A cursor reads them out as int16. The
 * stored layout is sound_format.h.
 *
 * No allocation and no ESP-IDF.
 */
#pragma once

#include <stdint.h>
#include "asset/asset_bytes.h"
#include "asset/asset_pack.h"
#include "audio/audio_adpcm.h"
#include "audio/audio_out.h"
#include "sound/sound_format.h"

/* The stored rate every sound is baked at: the output rate, so playback never resamples. */
#define SOUND_SAMPLE_RATE_HZ AUDIO_OUT_SAMPLE_RATE_HZ

typedef enum { SOUND_PCM16 = SOUND_FORMAT_PCM16, SOUND_IMA_ADPCM = SOUND_FORMAT_IMA_ADPCM } sound_format_t;

typedef struct {
    const uint8_t* data;   /* PCM16 LE samples or the first ADPCM block header; pack-owned. */
    uint32_t frames;       /* Stored mono sample count. */
    uint32_t loop_start;   /* First frame of the loop. */
    uint32_t loop_end;     /* Exclusive loop end; zero means one-shot. */
    uint16_t block_frames; /* Samples per ADPCM block; zero for PCM16. */
    sound_format_t format; /* Sample encoding at data. */
} sound_t;

typedef struct {
    const sound_t* sound; /* Validated view; the view and its pack must outlive this cursor. */
    uint32_t position;    /* Read-only next stored frame, for progress displays. */
    audio_adpcm_t adpcm;  /* Current block's codes and IMA state; unused for PCM16. */
} sound_cursor_t;

/* ADPCM seeks must be block-aligned, except the end position; failures zero the cursor. */
asset_status_t sound_seek(sound_cursor_t* c, const sound_t* sound, uint32_t frame);
/* Copies up to frames samples, wrapping at loop_end; one-shots stop at frames. */
uint32_t sound_read(sound_cursor_t* c, int16_t* out, uint32_t frames);

/* One stored sample: signed little-endian 16-bit, as PCM16 data and an ADPCM
 * block's predictor hold it. */
static inline int16_t
sound_read_sample(const uint8_t* at) {
    const uint16_t value = asset_read_u16(at);
    return (int16_t)(value <= INT16_MAX ? (int)value : (int)value - (UINT16_MAX + 1));
}

asset_status_t sound_open(const asset_pack_t* pack, const char* name, sound_t* out);

#include "sound/sound.h"
#include "sound/sound_format.h"

#include "asset/asset_bytes.h"
#include "asset/asset_entry.h"
#include "audio/audio_adpcm.h"

static asset_status_t
read_header(asset_view_t entry, sound_t* out, uint32_t* blocks, uint32_t* stride) {
    const uint8_t format = entry.data[SOUND_AT_FORMAT];
    const uint32_t frames = asset_read_u32(entry.data + SOUND_AT_FRAMES);
    const uint32_t start = asset_read_u32(entry.data + SOUND_AT_LOOP_START);
    const uint32_t end = asset_read_u32(entry.data + SOUND_AT_LOOP_END);
    const uint16_t block_frames = asset_read_u16(entry.data + SOUND_AT_BLOCK_FRAMES);
    const uint32_t offset = asset_read_u32(entry.data + SOUND_AT_DATA);
    if (entry.data[SOUND_AT_CHANNELS] != SOUND_CHANNELS
        || asset_read_u32(entry.data + SOUND_AT_SAMPLE_RATE) != SOUND_SAMPLE_RATE_HZ
        || (format != SOUND_PCM16 && format != SOUND_IMA_ADPCM) || frames == 0
        || !asset_entry_zero(entry, SOUND_AT_RESERVED16, SOUND_AT_DATA)
        || (end == 0 ? start != 0 : start >= end || end > frames)
        || (format == SOUND_PCM16 ? block_frames != 0 : block_frames % 2U == 0)) {
        return ASSET_ERR_FORMAT;
    }
    if (format == SOUND_IMA_ADPCM && start % block_frames != 0) {
        return ASSET_ERR_FORMAT;
    }
    *blocks = format == SOUND_IMA_ADPCM ? (frames / block_frames) + (frames % block_frames != 0) : 0;
    *stride = format == SOUND_IMA_ADPCM ? audio_adpcm_block_bytes(block_frames) : 0;
    const uint64_t bytes = format == SOUND_PCM16 ? (uint64_t)frames * SOUND_PCM_BYTES : (uint64_t)*blocks * *stride;
    if (offset < SOUND_HEADER_SIZE || offset % SOUND_ALIGNMENT != 0 || (uint64_t)offset + bytes > entry.size) {
        return ASSET_ERR_BOUNDS;
    }
    if (!asset_entry_zero(entry, SOUND_HEADER_SIZE, offset)) {
        return ASSET_ERR_FORMAT;
    }
    *out = (sound_t){.data = entry.data + offset,
                     .frames = frames,
                     .loop_start = start,
                     .loop_end = end,
                     .block_frames = block_frames,
                     .format = (sound_format_t)format};
    return ASSET_OK;
}

static asset_status_t
check_blocks(const sound_t* sound, uint32_t blocks, uint32_t stride) {
    if (sound->format != SOUND_IMA_ADPCM) {
        return ASSET_OK;
    }
    for (uint32_t i = 0; i < blocks; i++) {
        const uint8_t* block = sound->data + (i * stride);
        if (block[AUDIO_ADPCM_AT_STEP_INDEX] > AUDIO_ADPCM_STEP_INDEX_MAX || block[AUDIO_ADPCM_AT_RESERVED] != 0) {
            return ASSET_ERR_FORMAT;
        }
    }
    return ASSET_OK;
}

static asset_status_t
parse(asset_view_t entry, void* out) {
    sound_t* sound = out;
    uint32_t blocks = 0;
    uint32_t stride = 0;
    const asset_status_t status = read_header(entry, sound, &blocks, &stride);
    return status == ASSET_OK ? check_blocks(sound, blocks, stride) : status;
}

static const asset_entry_format_t FORMAT = {SOUND_ASSET, SOUND_VERSION, SOUND_AT_VERSION, SOUND_HEADER_SIZE,
                                            SOUND_ALIGNMENT};

asset_status_t
sound_open(const asset_pack_t* pack, const char* name, sound_t* out) {
    return asset_entry_read(pack, name, &FORMAT, parse, out, sizeof *out);
}

static void
open_block(sound_cursor_t* c) {
    const sound_t* sound = c->sound;
    const uint8_t* block =
        sound->data + ((c->position / sound->block_frames) * audio_adpcm_block_bytes(sound->block_frames));
    c->adpcm = (audio_adpcm_t){.codes = block + AUDIO_ADPCM_BLOCK_HEADER_BYTES,
                               .predictor = sound_read_sample(block),
                               .step_index = (int8_t)block[AUDIO_ADPCM_AT_STEP_INDEX]};
}

asset_status_t
sound_seek(sound_cursor_t* c, const sound_t* sound, uint32_t frame) {
    *c = (sound_cursor_t){0};
    if (frame > sound->frames
        || (sound->format == SOUND_IMA_ADPCM && frame != sound->frames && frame % sound->block_frames != 0)) {
        return ASSET_ERR_FORMAT;
    }
    c->sound = sound;
    c->position = frame;
    return ASSET_OK;
}

static void
read_run(sound_cursor_t* c, int16_t* out, uint32_t frames) {
    const sound_t* sound = c->sound;
    if (sound->format == SOUND_PCM16) {
        for (uint32_t i = 0; i < frames; i++) {
            out[i] = sound_read_sample(sound->data + ((c->position + i) * SOUND_PCM_BYTES));
        }
        c->position += frames;
        return;
    }
    uint32_t remaining = frames;
    while (remaining != 0) {
        uint32_t in_block = c->position % sound->block_frames;
        if (in_block == 0) {
            open_block(c);
            *out++ = c->adpcm.predictor;
            c->position++;
            remaining--;
            in_block++;
        }
        const uint32_t block_left = sound->block_frames - in_block;
        const uint32_t run = remaining < block_left ? remaining : block_left;
        audio_adpcm_read(&c->adpcm, out, run);
        out += run;
        c->position += run;
        remaining -= run;
    }
}

uint32_t
sound_read(sound_cursor_t* c, int16_t* out, uint32_t frames) {
    if (c->sound == NULL) {
        return 0;
    }
    const sound_t* sound = c->sound;
    uint32_t remaining = frames;
    while (remaining != 0) {
        if (sound->loop_end != 0 && c->position == sound->loop_end) {
            sound_seek(c, sound, sound->loop_start);
        }
        const uint32_t loop_end = sound_end_frame(sound);
        const uint32_t end = c->position < loop_end ? loop_end : sound->frames;
        const uint32_t available = end - c->position;
        const uint32_t count = remaining < available ? remaining : available;
        if (count == 0) {
            break;
        }
        read_run(c, out, count);
        out += count;
        remaining -= count;
    }
    return frames - remaining;
}

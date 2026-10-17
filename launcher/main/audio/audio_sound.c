#include "audio/audio_sound.h"

#include "asset/asset_bytes.h"
#include "asset/asset_entry.h"
#include "audio/audio_adpcm.h"

static asset_status_t
parse(asset_view_t entry, void* out) {
    const uint8_t format = entry.data[AUDIO_SOUND_AT_FORMAT];
    const uint32_t frames = asset_read_u32(entry.data + AUDIO_SOUND_AT_FRAMES);
    const uint32_t start = asset_read_u32(entry.data + AUDIO_SOUND_AT_LOOP_START);
    const uint32_t end = asset_read_u32(entry.data + AUDIO_SOUND_AT_LOOP_END);
    const uint16_t block_frames = asset_read_u16(entry.data + AUDIO_SOUND_AT_BLOCK_FRAMES);
    const uint32_t offset = asset_read_u32(entry.data + AUDIO_SOUND_AT_DATA);
    if (entry.data[AUDIO_SOUND_AT_CHANNELS] != AUDIO_SOUND_CHANNELS
        || asset_read_u32(entry.data + AUDIO_SOUND_AT_SAMPLE_RATE) != AUDIO_SOUND_SAMPLE_RATE_HZ
        || (format != AUDIO_SOUND_PCM16 && format != AUDIO_SOUND_IMA_ADPCM) || frames == 0
        || !asset_entry_zero(entry, AUDIO_SOUND_AT_RESERVED16, AUDIO_SOUND_AT_DATA)
        || (end == 0 ? start != 0 : start >= end || end > frames)
        || (format == AUDIO_SOUND_PCM16 ? block_frames != 0 : block_frames % 2U == 0)) {
        return ASSET_ERR_FORMAT;
    }
    if (format == AUDIO_SOUND_IMA_ADPCM && start % block_frames != 0) {
        return ASSET_ERR_FORMAT;
    }
    const uint32_t blocks = format == AUDIO_SOUND_IMA_ADPCM ? frames / block_frames + (frames % block_frames != 0) : 0;
    const uint32_t stride = format == AUDIO_SOUND_IMA_ADPCM ? audio_adpcm_block_bytes(block_frames) : 0;
    const uint64_t bytes =
        format == AUDIO_SOUND_PCM16 ? (uint64_t)frames * AUDIO_SOUND_PCM_BYTES : (uint64_t)blocks * stride;
    if (offset < AUDIO_SOUND_HEADER_SIZE || offset % AUDIO_SOUND_ALIGNMENT != 0
        || (uint64_t)offset + bytes > entry.size) {
        return ASSET_ERR_BOUNDS;
    }
    if (!asset_entry_zero(entry, AUDIO_SOUND_HEADER_SIZE, offset)) {
        return ASSET_ERR_FORMAT;
    }
    for (uint32_t i = 0; i < blocks; i++) {
        const uint8_t* block = entry.data + offset + i * stride;
        if (block[AUDIO_ADPCM_AT_STEP_INDEX] > AUDIO_ADPCM_STEP_INDEX_MAX || block[AUDIO_ADPCM_AT_RESERVED] != 0) {
            return ASSET_ERR_FORMAT;
        }
    }
    *(audio_sound_t*)out = (audio_sound_t){.data = entry.data + offset,
                                           .frames = frames,
                                           .loop_start = start,
                                           .loop_end = end,
                                           .block_frames = block_frames,
                                           .format = (audio_sound_format_t)format};
    return ASSET_OK;
}

static const asset_entry_format_t FORMAT = {AUDIO_SOUND_ASSET, AUDIO_SOUND_VERSION, AUDIO_SOUND_AT_VERSION,
                                            AUDIO_SOUND_HEADER_SIZE, AUDIO_SOUND_ALIGNMENT};

asset_status_t
audio_sound_open(const asset_pack_t* pack, const char* name, audio_sound_t* out) {
    return asset_entry_read(pack, name, &FORMAT, parse, out, sizeof *out);
}

static void
open_block(audio_sound_cursor_t* c) {
    const audio_sound_t* sound = c->sound;
    const uint8_t* block =
        sound->data + c->position / sound->block_frames * audio_adpcm_block_bytes(sound->block_frames);
    c->adpcm = (audio_adpcm_t){.codes = block + AUDIO_ADPCM_BLOCK_HEADER_BYTES,
                               .predictor = audio_adpcm_read_i16(block),
                               .step_index = (int8_t)block[AUDIO_ADPCM_AT_STEP_INDEX]};
}

asset_status_t
audio_sound_seek(audio_sound_cursor_t* c, const audio_sound_t* sound, uint32_t frame) {
    *c = (audio_sound_cursor_t){0};
    if (frame > sound->frames
        || (sound->format == AUDIO_SOUND_IMA_ADPCM && frame != sound->frames && frame % sound->block_frames != 0)) {
        return ASSET_ERR_FORMAT;
    }
    c->sound = sound;
    c->position = frame;
    if (sound->format == AUDIO_SOUND_IMA_ADPCM && frame < sound->frames) {
        open_block(c);
    }
    return ASSET_OK;
}

uint32_t
audio_sound_read(audio_sound_cursor_t* c, int16_t* out, uint32_t frames) {
    if (c->sound == NULL) {
        return 0;
    }
    const audio_sound_t* sound = c->sound;
    const uint32_t available = sound->frames - c->position;
    const uint32_t count = frames < available ? frames : available;
    if (sound->format == AUDIO_SOUND_PCM16) {
        for (uint32_t i = 0; i < count; i++) {
            out[i] = audio_adpcm_read_i16(sound->data + (c->position + i) * AUDIO_SOUND_PCM_BYTES);
        }
        c->position += count;
        return count;
    }
    uint32_t remaining = count;
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
    return count;
}

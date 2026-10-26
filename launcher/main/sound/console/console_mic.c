/* console_mic: capture-scoped PCM and codec state over the console transport. */
#include <stdio.h>
#include <string.h>
#include "audio/audio_codec.h"
#include "audio/audio_in.h"
#include "console/console.h"
#include "console/console_base64.h"
#include "console/console_verbs.h"
#include "core/memory.h"
#include "sound/console/console_mic_parse.h"
#include "sound/sound_engine.h"

#define MIC_RECORD_JSON_ENTRY_BYTES  32u
#define MIC_RECORD_JSON_HEADER_BYTES 128u
#define MIC_RECORD_REG_COUNT         (MIC_RECORD_LAST_REG - MIC_RECORD_FIRST_REG + 1u)

static bool
record(int16_t* pcm, size_t count, uint8_t* flags, size_t settle) {
    int16_t discard[AUDIO_CODEC_BLOCK_SAMPLES];
    while (settle) {
        const size_t n = settle < AUDIO_CODEC_BLOCK_SAMPLES ? settle : AUDIO_CODEC_BLOCK_SAMPLES;
        if (!audio_in_read(discard, n)) {
            return false;
        }
        settle -= n;
    }
    for (size_t pos = 0, block = 0; pos < count; block++) {
        const size_t n = count - pos < AUDIO_CODEC_BLOCK_SAMPLES ? count - pos : AUDIO_CODEC_BLOCK_SAMPLES;
        if (!audio_in_read(pcm + pos, n) || !audio_codec_register(MIC_RECORD_AUTOMUTE_REG, false, flags + block)) {
            return false;
        }
        pos += n;
    }
    return true;
}

static void
console_verb_mic(const char* args, console_reply_fn reply) {
    console_mic_args_t parsed;
    if (!console_mic_parse(args, &parsed)) {
        reply(MIC_RECORD_ERROR "usage: mic record <ms> [settle_ms]");
        return;
    }
    const size_t count = audio_out_samples(parsed.ms);
    const size_t blocks = (count + AUDIO_CODEC_BLOCK_SAMPLES - 1u) / AUDIO_CODEC_BLOCK_SAMPLES;
    const size_t json_bytes =
        MIC_RECORD_JSON_HEADER_BYTES + (blocks + MIC_RECORD_REG_COUNT) * MIC_RECORD_JSON_ENTRY_BYTES;
    const size_t line_bytes = (size_t)console_base64_encoded_len(AUDIO_CODEC_BLOCK_SAMPLES * sizeof(int16_t)) + 1u;
    char* line = memory_alloc(line_bytes, MEMORY_PSRAM);
    int16_t* pcm = memory_alloc(count * sizeof *pcm, MEMORY_PSRAM);
    uint8_t* flags = memory_alloc(blocks, MEMORY_PSRAM);
    char* json = memory_alloc(json_bytes, MEMORY_PSRAM);
    bool opened = false;
    bool ok = pcm && flags && json && line;
    if (ok) {
        opened = sound_capture_open();
        ok = opened;
    }
    if (ok) {
        ok = record(pcm, count, flags, audio_out_samples(parsed.settle_ms));
    }
    if (ok) {
        size_t used = (size_t)snprintf(json, json_bytes, "{\"rate\":%u,\"samples\":%u,\"automute\":[",
                                       AUDIO_OUT_SAMPLE_RATE_HZ, (unsigned)count);
        bool comma = false;
        for (size_t block = 0; block < blocks;) {
            if (!(flags[block] & MIC_RECORD_AUTOMUTE_MASK)) {
                block++;
                continue;
            }
            const size_t first = block++;
            while (block < blocks && (flags[block] & MIC_RECORD_AUTOMUTE_MASK)) {
                block++;
            }
            used += (size_t)snprintf(json + used, json_bytes - used, "%s[%u,%u]", comma ? "," : "", (unsigned)first,
                                     (unsigned)(block - first));
            comma = true;
        }
        used += (size_t)snprintf(json + used, json_bytes - used, "],\"regs\":{");
        for (unsigned reg = MIC_RECORD_FIRST_REG; reg <= MIC_RECORD_LAST_REG; reg++) {
            uint8_t value = 0;
            if (!audio_codec_register((uint8_t)reg, false, &value)) {
                ok = false;
                break;
            }
            used += (size_t)snprintf(json + used, json_bytes - used, "%s\"0x%02X\":%u",
                                     reg == MIC_RECORD_FIRST_REG ? "" : ",", reg, value);
        }
        (void)snprintf(json + used, json_bytes - used, "}}");
    }
    if (ok) {
        (void)console_mic_begin(line, line_bytes, AUDIO_OUT_SAMPLE_RATE_HZ, (unsigned)count);
        console_emit_line(line, "");
        for (size_t pos = 0; pos < count;) {
            const size_t n = count - pos < AUDIO_CODEC_BLOCK_SAMPLES ? count - pos : AUDIO_CODEC_BLOCK_SAMPLES;
            console_base64_encode((const uint8_t*)(pcm + pos), (int32_t)(n * sizeof *pcm), line);
            line[console_base64_encoded_len((int32_t)(n * sizeof *pcm))] = '\0';
            console_emit_line(MIC_RECORD_DATA, line);
            pos += n;
        }
        console_emit_line(MIC_RECORD_STATE, json);
    }
    if (opened) {
        ok = sound_capture_close() && ok;
    }
    if (ok) {
        console_emit_line(MIC_RECORD_END, "");
    } else {
        reply(MIC_RECORD_ERROR "capture failed");
    }
    memory_free(line);
    memory_free(json);
    memory_free(flags);
    memory_free(pcm);
}
CONSOLE_VERB(mic, sizeof "record 10000 10000" - 1, console_verb_mic)

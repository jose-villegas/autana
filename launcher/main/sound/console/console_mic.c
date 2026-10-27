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

#define MIC_RECORD_JSON_ENTRY_BYTES  32U
#define MIC_RECORD_JSON_HEADER_BYTES 128U
#define MIC_RECORD_REG_COUNT         (MIC_RECORD_LAST_REG - MIC_RECORD_FIRST_REG + 1U)

static bool
apply_writes(console_mic_args_t* args, size_t pos, size_t block, unsigned* applied) {
    for (unsigned i = 0; i < args->write_count; i++) {
        console_mic_write_t* write = &args->writes[i];
        const unsigned bit = 1U << i;
        if (!(*applied & bit) && pos >= audio_out_samples(write->ms)) {
            uint8_t value = write->value;
            if (!audio_codec_register(write->reg, true, &value) || value != write->value) {
                return false;
            }
            write->block = (uint32_t)block;
            *applied |= bit;
        }
    }
    return true;
}

static bool
record(int16_t* pcm, size_t count, uint8_t* flags, size_t settle, console_mic_args_t* args) {
    int16_t discard[AUDIO_CODEC_BLOCK_SAMPLES];
    while (settle) {
        const size_t n = settle < AUDIO_CODEC_BLOCK_SAMPLES ? settle : AUDIO_CODEC_BLOCK_SAMPLES;
        if (!audio_in_read(discard, n)) {
            return false;
        }
        settle -= n;
    }
    unsigned applied = 0;
    for (size_t pos = 0, block = 0; pos < count; block++) {
        const size_t n = count - pos < AUDIO_CODEC_BLOCK_SAMPLES ? count - pos : AUDIO_CODEC_BLOCK_SAMPLES;
        if (!apply_writes(args, pos, block, &applied) || !audio_in_read(pcm + pos, n)
            || !audio_codec_register(MIC_RECORD_AUTOMUTE_REG, false, flags + block)) {
            return false;
        }
        pos += n;
    }
    return applied == (1U << args->write_count) - 1U;
}

/* Appends the automute flag as [first_block, run_length] runs. */
static size_t
append_automute_runs(char* json, size_t json_bytes, size_t used, const uint8_t* flags, size_t blocks) {
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
    return used;
}

static bool
format_state(char* json, size_t json_bytes, const uint8_t* flags, size_t blocks, size_t count,
             const console_mic_args_t* args) {
    size_t used = (size_t)snprintf(json, json_bytes, "{\"rate\":%u,\"samples\":%u,\"automute\":[",
                                   AUDIO_OUT_SAMPLE_RATE_HZ, (unsigned)count);
    used = append_automute_runs(json, json_bytes, used, flags, blocks);
    used += (size_t)snprintf(json + used, json_bytes - used, "],\"regs\":{");
    for (unsigned reg = MIC_RECORD_FIRST_REG; reg <= MIC_RECORD_LAST_REG; reg++) {
        uint8_t value = 0;
        if (!audio_codec_register((uint8_t)reg, false, &value)) {
            return false;
        }
        used += (size_t)snprintf(json + used, json_bytes - used, "%s\"0x%02X\":%u",
                                 reg == MIC_RECORD_FIRST_REG ? "" : ",", reg, value);
    }
    used += (size_t)snprintf(json + used, json_bytes - used, "},\"writes\":[");
    for (unsigned i = 0; i < args->write_count; i++) {
        const console_mic_write_t* write = &args->writes[i];
        used += (size_t)snprintf(json + used, json_bytes - used, "%s[%u,%u,%u]", i ? "," : "", (unsigned)write->block,
                                 write->reg, write->value);
    }
    (void)snprintf(json + used, json_bytes - used, "]}");
    return true;
}

static void
emit_capture(char* line, size_t line_bytes, const int16_t* pcm, size_t count, const char* json) {
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

static void
console_verb_mic(const char* args, console_reply_fn reply) {
    console_mic_args_t parsed;
    if (!console_mic_parse(args, &parsed)) {
        reply(MIC_RECORD_ERROR "usage: mic record <ms> [settle_ms] [at <ms> <reg> <value>]...");
        return;
    }
    const size_t count = audio_out_samples(parsed.ms);
    const size_t blocks = (count + AUDIO_CODEC_BLOCK_SAMPLES - 1U) / AUDIO_CODEC_BLOCK_SAMPLES;
    const size_t json_bytes = MIC_RECORD_JSON_HEADER_BYTES
                              + ((blocks + MIC_RECORD_REG_COUNT + MIC_RECORD_WRITES_MAX) * MIC_RECORD_JSON_ENTRY_BYTES);
    const size_t line_bytes = (size_t)console_base64_encoded_len(AUDIO_CODEC_BLOCK_SAMPLES * sizeof(int16_t)) + 1U;
    char* line = memory_alloc(line_bytes, MEMORY_PSRAM);
    int16_t* pcm = memory_alloc(count * sizeof *pcm, MEMORY_PSRAM);
    uint8_t* flags = memory_alloc(blocks, MEMORY_PSRAM);
    char* json = memory_alloc(json_bytes, MEMORY_PSRAM);
    const bool opened = pcm && flags && json && line && sound_capture_open();
    bool ok = opened && record(pcm, count, flags, audio_out_samples(parsed.settle_ms), &parsed)
              && format_state(json, json_bytes, flags, blocks, count, &parsed);
    if (ok) {
        emit_capture(line, line_bytes, pcm, count, json);
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
CONSOLE_VERB_LONG(mic, MIC_RECORD_ARGS_MAX, MIC_RECORD_LINE_MAX, console_verb_mic)

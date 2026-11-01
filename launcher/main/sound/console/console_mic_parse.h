/* console_mic_parse: bounded capture arguments and wire constants. */
#pragma once
#include <stdio.h>
#include <string.h>
#include "audio/audio_loopback.h"
#include "audio/audio_out.h"
#include "audio/console/console_codec_parse.h"
#include "console/console_number.h"
#define MIC_RECORD_BEGIN_BYTES   sizeof(MIC_RECORD_BEGIN "{\"rate\":4294967295,\"samples\":4294967295}")
#define MIC_RECORD_MAX_MS        10000u
#define MIC_RECORD_FIRST_REG     0x14u
#define MIC_RECORD_LAST_REG      0x37u
#define MIC_RECORD_AUTOMUTE_REG  0xFCu
#define MIC_RECORD_AUTOMUTE_MASK 2u
#define MIC_RECORD_BEGIN         "MIC_BEGIN "
#define MIC_RECORD_STATE         "MIC_STATE "
#define MIC_RECORD_END           "MIC_END"
#define MIC_RECORD_DATA          "MIC_DATA "
#define MIC_RECORD_ERROR         "MIC error: "

#define MIC_RECORD_WRITES_MAX    4u
#define MIC_RECORD_TONES_MAX     10u
#define MIC_RECORD_ENTRIES_MAX   (MIC_RECORD_WRITES_MAX + MIC_RECORD_TONES_MAX)
#define MIC_RECORD_BYTE_MAX      255u
#define MIC_RECORD_ARGS_MAX                                                                                            \
    (sizeof("record 10000 10000") - 1u + MIC_RECORD_ENTRIES_MAX * (sizeof(" at -10000 tone 11024 60000 100") - 1u))
#define MIC_RECORD_LINE_MAX (sizeof("mic ") + MIC_RECORD_ARGS_MAX)

typedef enum { CONSOLE_MIC_WRITE, CONSOLE_MIC_TONE } console_mic_entry_kind_t;

typedef struct {
    int32_t ms, block;
    console_mic_entry_kind_t kind;

    union {
        struct {
            uint8_t reg, value;
        } write;

        struct {
            uint32_t hz, duration_ms, gain;
        } tone;
    };
} console_mic_entry_t;

typedef struct {
    uint32_t ms, settle_ms;
    unsigned entry_count;
    console_mic_entry_t entries[MIC_RECORD_ENTRIES_MAX];
} console_mic_args_t;

static inline bool
console_mic_apply_entries(console_mic_args_t* args, int32_t pos, int32_t block, unsigned* applied,
                          bool (*apply)(const console_mic_entry_t*)) {
    for (unsigned i = 0; i < args->entry_count; i++) {
        console_mic_entry_t* entry = &args->entries[i];
        const unsigned bit = 1U << i;
        int32_t at = (int32_t)audio_out_samples((uint32_t)(entry->ms < 0 ? -entry->ms : entry->ms));
        if (entry->ms < 0) {
            at = -at;
        }
        if (!(*applied & bit) && pos >= at) {
            if (!apply(entry)) {
                return false;
            }
            entry->block = block;
            *applied |= bit;
        }
    }
    return true;
}

static inline bool
console_mic_tone_parse(const char** text, console_mic_entry_t* entry, uint32_t remaining_ms) {
    return console_u32(text, AUDIO_OUT_NYQUIST_HZ - 1u, false, &entry->tone.hz)
           && console_u32(text, AUDIO_OUT_MAX_MS, false, &entry->tone.duration_ms)
           && console_u32(text, AUDIO_OUT_VOLUME_MAX_PCT, false, &entry->tone.gain)
           && audio_out_args_valid(entry->tone.hz, entry->tone.duration_ms, entry->tone.gain)
           && entry->tone.duration_ms <= remaining_ms;
}

static inline bool
console_mic_entry_parse(const char** text, console_mic_args_t* out) {
    console_mic_entry_t entry = {0};
    if (strncmp(*text, "at ", sizeof("at ") - 1) != 0 || out->entry_count == MIC_RECORD_ENTRIES_MAX) {
        return false;
    }
    *text += sizeof("at ") - 1;
    *text = console_token_skip(*text);
    const bool negative = **text == '-';
    *text += negative;
    uint32_t offset;
    if ((negative && (**text < '0' || **text > '9'))
        || !console_u32(text, negative ? out->settle_ms : out->ms - 1u, false, &offset)) {
        return false;
    }
    entry.ms = negative ? -(int32_t)offset : (int32_t)offset;
    if (strncmp(*text, "tone ", sizeof("tone ") - 1) == 0) {
        entry.kind = CONSOLE_MIC_TONE;
        *text += sizeof("tone ") - 1;
        if (!console_mic_tone_parse(text, &entry, (uint32_t)((int32_t)out->ms - entry.ms))) {
            return false;
        }
    } else if (!console_codec_byte(text, &entry.write.reg) || !console_codec_byte(text, &entry.write.value)) {
        return false;
    }
    unsigned same_kind = 0;
    for (unsigned i = 0; i < out->entry_count; i++) {
        same_kind += out->entries[i].kind == entry.kind;
    }
    if (same_kind == (entry.kind == CONSOLE_MIC_TONE ? MIC_RECORD_TONES_MAX : MIC_RECORD_WRITES_MAX)) {
        return false;
    }
    out->entries[out->entry_count++] = entry;
    return true;
}

static inline bool
console_mic_parse(const char* args, console_mic_args_t* out) {
    const char prefix[] = "record ";
    if (strncmp(args, prefix, sizeof prefix - 1) != 0) {
        return false;
    }
    args += sizeof prefix - 1;
    console_mic_args_t parsed = {.settle_ms = AUDIO_LOOPBACK_SETTLE_MS};
    if (!console_u32(&args, MIC_RECORD_MAX_MS, false, &parsed.ms) || !parsed.ms) {
        return false;
    }
    if (*args && *args != 'a' && !console_u32(&args, MIC_RECORD_MAX_MS, false, &parsed.settle_ms)) {
        return false;
    }
    while (*args) {
        if (!console_mic_entry_parse(&args, &parsed)) {
            return false;
        }
    }
    *out = parsed;
    return true;
}

static inline bool
console_mic_begin(char* out, size_t capacity, unsigned rate, unsigned samples) {
    if (capacity < MIC_RECORD_BEGIN_BYTES) {
        return false;
    }
    const int n = snprintf(out, capacity, MIC_RECORD_BEGIN "{\"rate\":%u,\"samples\":%u}", rate, samples);
    return n >= 0 && (size_t)n < capacity;
}

/* console_mic_parse: bounded capture arguments and wire constants. */
#pragma once
#include <stdio.h>
#include <string.h>
#include "audio/audio_loopback.h"
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
#define MIC_RECORD_BYTE_MAX      255u
#define MIC_RECORD_ARGS_MAX                                                                                            \
    (sizeof("record 10000 10000") - 1u + MIC_RECORD_WRITES_MAX * (sizeof(" at 10000 0xFF 0xFF") - 1u))

#define MIC_RECORD_LINE_MAX (sizeof("mic ") + MIC_RECORD_ARGS_MAX)

typedef struct {
    uint32_t ms, block;
    uint8_t reg, value;
} console_mic_write_t;

typedef struct {
    uint32_t ms, settle_ms;
    unsigned write_count;
    console_mic_write_t writes[MIC_RECORD_WRITES_MAX];
} console_mic_args_t;

static inline bool
console_mic_number(const char** text, unsigned max, uint32_t* out) {
    return console_u32(text, max, false, out);
}

static inline bool
console_mic_write_parse(const char** text, console_mic_args_t* out) {
    uint32_t ms;
    uint8_t reg, value;
    if (strncmp(*text, "at ", sizeof("at ") - 1) != 0 || out->write_count == MIC_RECORD_WRITES_MAX) {
        return false;
    }
    *text += sizeof("at ") - 1;
    if (!console_mic_number(text, out->ms - 1u, &ms) || !console_codec_byte(text, &reg)
        || !console_codec_byte(text, &value)) {
        return false;
    }
    out->writes[out->write_count++] = (console_mic_write_t){.ms = ms, .reg = reg, .value = value};
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
    if (!console_mic_number(&args, MIC_RECORD_MAX_MS, &parsed.ms) || !parsed.ms) {
        return false;
    }
    if (*args && *args != 'a' && !console_mic_number(&args, MIC_RECORD_MAX_MS, &parsed.settle_ms)) {
        return false;
    }
    while (*args) {
        if (!console_mic_write_parse(&args, &parsed)) {
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

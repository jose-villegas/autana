/* console_mic_parse: bounded capture arguments and wire constants. */
#pragma once
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "audio/audio_loopback.h"
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

typedef struct {
    uint32_t ms, settle_ms;
} console_mic_args_t;

static inline bool
console_mic_parse(const char* args, console_mic_args_t* out) {
    const char prefix[] = "record ";
    if (strncmp(args, prefix, sizeof prefix - 1) != 0) {
        return false;
    }
    args += sizeof prefix - 1;
    unsigned long values[2] = {0, AUDIO_LOOPBACK_SETTLE_MS};
    unsigned count = 0;
    while (*args) {
        args += strspn(args, " \t\r\n");
        if (!*args) {
            break;
        }
        if (count == 2 || *args < '0' || *args > '9') {
            return false;
        }
        char* end;
        errno = 0;
        values[count] = strtoul(args, &end, 10);
        if (errno || values[count] > MIC_RECORD_MAX_MS || (*end && !strchr(" \t\r\n", *end))) {
            return false;
        }
        count++;
        args = end;
    }
    if (!count || !values[0]) {
        return false;
    }
    *out = (console_mic_args_t){(uint32_t)values[0], (uint32_t)values[1]};
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

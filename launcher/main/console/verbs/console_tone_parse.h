/* Decimal tone arguments, with limits owned by the audio output. */
#pragma once
#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "audio/audio_out.h"

#define CONSOLE_TONE_DEFAULT_VOLUME_PCT 30u
#define CONSOLE_TONE_INTERNAL           "internal"

typedef struct {
    uint16_t hz;
    uint16_t ms;
    uint8_t volume_pct;
    bool internal;
} console_tone_args_t;

static inline bool
console_tone_parse(const char* args, console_tone_args_t* out) {
    unsigned long long hz, ms, volume = CONSOLE_TONE_DEFAULT_VOLUME_PCT;
    size_t length = strlen(args);
    while (length && isspace((unsigned char)args[length - 1])) {
        length--;
    }
    size_t token = length;
    while (token && !isspace((unsigned char)args[token - 1])) {
        token--;
    }
    const bool internal = length - token == sizeof CONSOLE_TONE_INTERNAL - 1
                          && strncmp(args + token, CONSOLE_TONE_INTERNAL, length - token) == 0;
    length = internal ? token : strlen(args);
    if (strspn(args, "0123456789 \t\r\n\v\f") != length) {
        return false;
    }
    errno = 0;
    int consumed = 0;
    int fields = sscanf(args, "%llu %llu %n", &hz, &ms, &consumed);
    if (fields == 2 && (size_t)consumed < length) {
        int extra = 0;
        fields += sscanf(args + consumed, "%llu %n", &volume, &extra);
        consumed += extra;
    }
    if ((size_t)consumed != length || errno == ERANGE || (fields != 2 && fields != 3) || hz > UINT32_MAX
        || ms > UINT32_MAX || volume > UINT32_MAX
        || !audio_out_args_valid((uint32_t)hz, (uint32_t)ms, (uint32_t)volume)) {
        return false;
    }
    *out = (console_tone_args_t){
        .hz = (uint16_t)hz, .ms = (uint16_t)ms, .volume_pct = (uint8_t)volume, .internal = internal};
    return true;
}

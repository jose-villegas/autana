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
console_tone_parse_options(const char* args, console_tone_args_t* out) {
    unsigned long long volume = CONSOLE_TONE_DEFAULT_VOLUME_PCT;
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
    if (strspn(args, " \t\r\n\v\f") < length && sscanf(args, "%llu %n", &volume, &consumed) != 1) {
        return false;
    }
    if (!consumed) {
        consumed = (int)strspn(args, " \t\r\n\v\f");
    }
    if ((size_t)consumed != length || errno == ERANGE || volume > AUDIO_OUT_VOLUME_MAX_PCT) {
        return false;
    }
    out->volume_pct = (uint8_t)volume;
    out->internal = internal;
    return true;
}

static inline bool
console_tone_parse(const char* args, console_tone_args_t* out) {
    unsigned long long hz, ms;
    int consumed = 0;
    errno = 0;
    if (sscanf(args, "%llu %llu%n", &hz, &ms, &consumed) != 2 || errno == ERANGE || hz > UINT32_MAX || ms > UINT32_MAX
        || strspn(args, "0123456789 \t\r\n\v\f") < (size_t)consumed
        || (args[consumed] && !isspace((unsigned char)args[consumed]))
        || !audio_out_args_valid((uint32_t)hz, (uint32_t)ms, CONSOLE_TONE_DEFAULT_VOLUME_PCT)) {
        return false;
    }
    console_tone_args_t parsed = {.hz = (uint16_t)hz, .ms = (uint16_t)ms};
    if (!console_tone_parse_options(args + consumed, &parsed)) {
        return false;
    }
    *out = parsed;
    return true;
}

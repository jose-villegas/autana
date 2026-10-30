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
#include "input/motion/imu.h"

#define CONSOLE_TONE_PULSE_WINDOW_MAX_MS 500u
#define CONSOLE_TONE_DEFAULT_VOLUME_PCT  30u
#define CONSOLE_TONE_PULSE               "pulse"
#define CONSOLE_TONE_INTERNAL            "internal"

typedef struct {
    uint16_t hz;
    uint16_t ms;
    uint16_t pulse_ms;
    uint8_t volume_pct;
    bool internal;
} console_tone_args_t;

static inline bool
console_tone_decimal(const char** args, unsigned long long maximum, unsigned long long* value) {
    int consumed = 0;
    errno = 0;
    if (!isdigit((unsigned char)**args) || sscanf(*args, "%llu%n", value, &consumed) != 1 || errno == ERANGE
        || *value > maximum) {
        return false;
    }
    *args += consumed;
    if (**args && !isspace((unsigned char)**args)) {
        return false;
    }
    *args += strspn(*args, " \t\r\n\v\f");
    return true;
}

static inline bool
console_tone_parse_options(const char* args, console_tone_args_t* out) {
    unsigned long long volume = CONSOLE_TONE_DEFAULT_VOLUME_PCT;
    unsigned long long pulse = 0;
    args += strspn(args, " \t\r\n\v\f");
    if (isdigit((unsigned char)*args) && !console_tone_decimal(&args, AUDIO_OUT_VOLUME_MAX_PCT, &volume)) {
        return false;
    }
    if (strncmp(args, CONSOLE_TONE_PULSE, sizeof CONSOLE_TONE_PULSE - 1) == 0) {
        args += sizeof CONSOLE_TONE_PULSE - 1;
        if (!isspace((unsigned char)*args)) {
            return false;
        }
        args += strspn(args, " \t\r\n\v\f");
        if (!console_tone_decimal(&args, out->ms, &pulse) || !pulse || out->ms > CONSOLE_TONE_PULSE_WINDOW_MAX_MS
            || out->ms < IMU_BUZZ_WINDOW_MS) {
            return false;
        }
    }
    const bool internal = strncmp(args, CONSOLE_TONE_INTERNAL, sizeof CONSOLE_TONE_INTERNAL - 1) == 0;
    if (internal) {
        args += sizeof CONSOLE_TONE_INTERNAL - 1;
    }
    if (strspn(args, " \t\r\n\v\f") != strlen(args)) {
        return false;
    }
    out->volume_pct = (uint8_t)volume;
    out->pulse_ms = (uint16_t)pulse;
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

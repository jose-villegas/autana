/* Decimal tone arguments, with limits owned by the audio output. */
#pragma once
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "audio/audio_out.h"

#define CONSOLE_TONE_DEFAULT_VOLUME_PCT 30u

typedef struct {
    uint16_t hz;
    uint16_t ms;
    uint8_t volume_pct;
} console_tone_args_t;

static inline bool
console_tone_parse(const char* args, console_tone_args_t* out) {
    unsigned long long hz, ms, volume = CONSOLE_TONE_DEFAULT_VOLUME_PCT;
    char trailing;
    if (strspn(args, "0123456789 \t\r\n\v\f") != strlen(args)) {
        return false;
    }
    errno = 0;
    const int fields = sscanf(args, "%llu %llu %llu %c", &hz, &ms, &volume, &trailing);
    if (errno == ERANGE || (fields != 2 && fields != 3) || hz > UINT32_MAX || ms > UINT32_MAX || volume > UINT32_MAX
        || !audio_out_args_valid((uint32_t)hz, (uint32_t)ms, (uint32_t)volume)) {
        return false;
    }
    *out = (console_tone_args_t){.hz = (uint16_t)hz, .ms = (uint16_t)ms, .volume_pct = (uint8_t)volume};
    return true;
}

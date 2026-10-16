/* Decimal tone arguments, with limits owned by the audio output. */
#pragma once
#include <ctype.h>
#include <stddef.h>
#define CONSOLE_TONE_DECIMAL_BASE 10u
#include <stdbool.h>
#include <stdint.h>
#include "audio/audio_out.h"
#define CONSOLE_TONE_DEFAULT_VOLUME_PCT 30u

typedef struct {
    uint16_t hz;
    uint16_t ms;
    uint8_t volume_pct;
} console_tone_args_t;

static inline bool
console_tone_parse(const char* args, console_tone_args_t* out) {
    const uint32_t limits[] = {AUDIO_OUT_NYQUIST_HZ - 1, AUDIO_OUT_MAX_MS, AUDIO_OUT_VOLUME_MAX_PCT};
    uint32_t values[] = {0, 0, CONSOLE_TONE_DEFAULT_VOLUME_PCT};
    size_t field = 0;
    while (isspace((unsigned char)*args)) {
        args++;
    }
    while (*args) {
        if (field >= sizeof values / sizeof values[0] || !isdigit((unsigned char)*args)) {
            return false;
        }
        uint32_t value = 0;
        while (isdigit((unsigned char)*args)) {
            const unsigned digit = (unsigned)(*args - '0');
            if (value > (limits[field] - digit) / CONSOLE_TONE_DECIMAL_BASE) {
                return false;
            }
            value = value * CONSOLE_TONE_DECIMAL_BASE + digit;
            args++;
        }
        if (*args && !isspace((unsigned char)*args)) {
            return false;
        }
        values[field++] = value;
        while (isspace((unsigned char)*args)) {
            args++;
        }
    }
    if (field < 2 || values[0] == 0 || values[1] == 0) {
        return false;
    }
    *out =
        (console_tone_args_t){.hz = (uint16_t)values[0], .ms = (uint16_t)values[1], .volume_pct = (uint8_t)values[2]};
    return true;
}

/* Decimal tone arguments, with limits owned by the audio output. */
#pragma once
#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "audio/audio_out.h"
#include "console/console_number.h"
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

/* [volume] [pulse <ms>] [internal]: each optional, in that order, ending the line. A pulse fits
 * inside the tone, whose window is long enough for the IMU buzz window and bounded. */
static inline bool
console_tone_parse_options(const char* args, console_tone_args_t* out) {
    uint32_t volume = CONSOLE_TONE_DEFAULT_VOLUME_PCT;
    uint32_t pulse = 0;
    if (isdigit((unsigned char)*console_token_skip(args))
        && !console_u32(&args, AUDIO_OUT_VOLUME_MAX_PCT, false, &volume)) {
        return false;
    }
    if (console_word(&args, CONSOLE_TONE_PULSE)
        && (!console_u32(&args, out->ms, false, &pulse) || !pulse || out->ms > CONSOLE_TONE_PULSE_WINDOW_MAX_MS
            || out->ms < IMU_BUZZ_WINDOW_MS)) {
        return false;
    }
    out->internal = console_word(&args, CONSOLE_TONE_INTERNAL);
    out->volume_pct = (uint8_t)volume;
    out->pulse_ms = (uint16_t)pulse;
    return *console_token_skip(args) == '\0';
}

static inline bool
console_tone_parse(const char* args, console_tone_args_t* out) {
    uint32_t hz, ms;
    if (!console_u32(&args, UINT32_MAX, false, &hz) || !console_u32(&args, UINT32_MAX, false, &ms)
        || !audio_out_args_valid(hz, ms, CONSOLE_TONE_DEFAULT_VOLUME_PCT)) {
        return false;
    }
    console_tone_args_t parsed = {.hz = (uint16_t)hz, .ms = (uint16_t)ms};
    if (!console_tone_parse_options(args, &parsed)) {
        return false;
    }
    *out = parsed;
    return true;
}

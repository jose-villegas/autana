/* Codec addresses and values are unsigned bytes in decimal or hexadecimal. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "console/console_number.h"

typedef struct {
    uint8_t reg;
    uint8_t value;
    bool write;
} console_codec_args_t;

static inline bool
console_codec_byte(const char** cursor, uint8_t* out) {
    uint32_t value;
    if (!console_u32(cursor, UINT8_MAX, true, &value)) {
        return false;
    }
    *out = (uint8_t)value;
    return true;
}

static inline bool
console_codec_parse(const char* args, console_codec_args_t* out) {
    console_codec_args_t parsed = {0};
    if (!console_codec_byte(&args, &parsed.reg)) {
        return false;
    }
    parsed.write = *args != '\0';
    if (parsed.write && !console_codec_byte(&args, &parsed.value)) {
        return false;
    }
    if (*args) {
        return false;
    }
    *out = parsed;
    return true;
}

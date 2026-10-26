/* Codec addresses and values are unsigned bytes in decimal or hexadecimal. */
#pragma once
#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#define CONSOLE_CODEC_DECIMAL_BASE 10
#define CONSOLE_CODEC_HEX_BASE     16

typedef struct {
    uint8_t reg;
    uint8_t value;
    bool write;
} console_codec_args_t;

static inline bool
console_codec_byte(const char** cursor, uint8_t* out) {
    const char* text = *cursor;
    while (isspace((unsigned char)*text)) {
        text++;
    }
    if (!isdigit((unsigned char)*text)) {
        return false;
    }
    const int base =
        text[0] == '0' && (text[1] == 'x' || text[1] == 'X') ? CONSOLE_CODEC_HEX_BASE : CONSOLE_CODEC_DECIMAL_BASE;
    errno = 0;
    char* end;
    const unsigned long value = strtoul(text, &end, base);
    if (end == text || errno == ERANGE || value > UINT8_MAX || (*end && !isspace((unsigned char)*end))) {
        return false;
    }
    while (isspace((unsigned char)*end)) {
        end++;
    }
    *cursor = end;
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

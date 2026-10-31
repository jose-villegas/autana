/* console_number: space-separated console tokens, read by math/text. A reader
 * skips leading space, wants the token to end at a space or the line's end,
 * then steps over the space after it; a failure leaves *cursor alone. */
#pragma once
#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "math/text/math_text.h"

static inline const char*
console_token_skip(const char* text) {
    while (isspace((unsigned char)*text)) {
        text++;
    }
    return text;
}

static inline bool
console_token_done(const char** cursor, const char* end, bool ok) {
    if (ok && (*end == '\0' || isspace((unsigned char)*end))) {
        *cursor = console_token_skip(end);
        return true;
    }
    return false;
}

static inline bool
console_word(const char** cursor, const char* word) {
    const char* text = console_token_skip(*cursor);
    const size_t length = strlen(word);
    return strncmp(text, word, length) == 0 && console_token_done(cursor, text + length, true);
}

/* An unsigned count up to max; with hex, 0x-prefixed hexadecimal also reads. */
static inline bool
console_u32(const char** cursor, uint32_t max, bool hex, uint32_t* out) {
    const char* text = console_token_skip(*cursor);
    const char* end = text;
    uint32_t value = 0;
    const bool prefixed = hex && text[0] == '0' && (text[1] == 'x' || text[1] == 'X');
    const bool ok = prefixed ? mathu_parse_hex(text, &end, &value) : mathu_parse(text, &end, &value);
    if (!console_token_done(cursor, end, ok && value <= max)) {
        return false;
    }
    *out = value;
    return true;
}

static inline bool
console_i32(const char** cursor, int32_t* out) {
    const char* text = console_token_skip(*cursor);
    const char* end = text;
    int32_t value = 0;
    const bool ok = mathi_parse(text, &end, &value);
    if (!console_token_done(cursor, end, ok)) {
        return false;
    }
    *out = value;
    return true;
}

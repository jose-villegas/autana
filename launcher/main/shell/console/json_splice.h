/*
 * json_splice, one more key on a JSON object already written: the closing
 * brace becomes `,"<key>":<fragment>}`, with no comma on an empty object.
 * Nothing parses the object; its last byte must be that brace.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/* False, with `json` untouched, when it does not end in `}` or the whole
 * fragment does not fit in `json_size`: a truncated fragment would break the
 * line for whoever parses it. */
static inline bool
json_splice_key(char* json, size_t json_size, const char* key, const char* fragment) {
    const size_t length = strlen(json);
    const size_t key_length = strlen(key);
    const size_t fragment_length = strlen(fragment);
    if (length == 0 || json[length - 1] != '}') {
        return false;
    }
    const bool first_key = length >= 2 && json[length - 2] == '{';
    const size_t added = (first_key ? 0 : 1) + 2 + key_length + 1 + fragment_length;
    if (length + added >= json_size) {
        return false;
    }
    char* at = json + length - 1;
    if (!first_key) {
        *at++ = ',';
    }
    *at++ = '"';
    memcpy(at, key, key_length);
    at += key_length;
    *at++ = '"';
    *at++ = ':';
    memcpy(at, fragment, fragment_length);
    at += fragment_length;
    *at++ = '}';
    *at = '\0';
    return true;
}

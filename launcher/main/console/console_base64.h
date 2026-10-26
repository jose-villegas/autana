/* console_base64: independent RFC 4648 chunks with padding and no terminator, for binary streams over the console. */
#pragma once
#include <stdint.h>

static inline int32_t
console_base64_encoded_len(int32_t len) {
    return ((len + 2) / 3) * 4;
}

static inline void
console_base64_encode(const uint8_t* in, int32_t len, char* out) {
    static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    int32_t i = 0, o = 0;
    for (; i + 3 <= len; i += 3) {
        const uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8) | in[i + 2];
        out[o++] = table[(v >> 18) & 0x3F];
        out[o++] = table[(v >> 12) & 0x3F];
        out[o++] = table[(v >> 6) & 0x3F];
        out[o++] = table[v & 0x3F];
    }

    const int32_t rem = len - i;
    if (rem == 1) {
        const uint32_t v = (uint32_t)in[i] << 16;
        out[o++] = table[(v >> 18) & 0x3F];
        out[o++] = table[(v >> 12) & 0x3F];
        out[o++] = '=';
        out[o++] = '=';
    } else if (rem == 2) {
        const uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8);
        out[o++] = table[(v >> 18) & 0x3F];
        out[o++] = table[(v >> 12) & 0x3F];
        out[o++] = table[(v >> 6) & 0x3F];
        out[o++] = '=';
    }
}

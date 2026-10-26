/* strbuf: text built by appending pieces into a caller's fixed buffer. A piece fits whole or is left out, so a full
 * buffer never ends in half a number, and the text stays terminated. */
#pragma once

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

typedef struct {
    char* text;
    size_t size; /* bytes, terminator included */
    size_t length;
    bool truncated; /* some piece did not fit and was left out */
} strbuf_t;

/* Continues text already `length` long in `size` bytes; a size of 0 holds nothing, not even the terminator. */
static inline strbuf_t
strbuf_at(char* text, size_t size, size_t length) {
    if (length < size) {
        text[length] = '\0';
    }
    return (strbuf_t){.text = text, .size = size, .length = length, .truncated = length >= size};
}

static inline strbuf_t
strbuf_make(char* text, size_t size) {
    return strbuf_at(text, size, 0);
}

static inline bool
strbuf_vappendf(strbuf_t* b, const char* format, va_list args) {
    if (b->length >= b->size) {
        b->truncated = true;
        return false;
    }
    const int wrote = vsnprintf(b->text + b->length, b->size - b->length, format, args);
    if (wrote < 0 || (size_t)wrote >= b->size - b->length) {
        b->text[b->length] = '\0';
        b->truncated = true;
        return false;
    }
    b->length += (size_t)wrote;
    return true;
}

static inline __attribute__((format(printf, 2, 3))) bool
strbuf_appendf(strbuf_t* b, const char* format, ...) {
    va_list args;
    va_start(args, format);
    const bool fits = strbuf_vappendf(b, format, args);
    va_end(args);
    return fits;
}

/* Names become stable numeric keys for lookup at initialization or load time. */
#pragma once

#include <stddef.h>
#include <stdint.h>

typedef uint32_t name_hash_t;

/* Reserved: bakers refuse stored names that hash to this value. */
#define NAME_HASH_NONE         0U
#define NAME_HASH_OFFSET_BASIS 0x811C9DC5U
#define NAME_HASH_PRIME        0x01000193U

/* FNV-1a 32 folded onto seed. Start with NAME_HASH_OFFSET_BASIS; pass the previous result to continue.
 * Fowler, Noll, Vo: FNV-1a uses unsigned bytes and modulo-32-bit arithmetic.
 * Inline: per-frame callers fold a few bytes at a time, so a call would cost more than the fold. */
static inline name_hash_t
name_hash_seeded_n(name_hash_t seed, const void* data, size_t size) {
    const unsigned char* bytes = data;
    for (size_t i = 0; i < size; i++) {
        seed ^= bytes[i];
        seed *= NAME_HASH_PRIME;
    }
    return seed;
}

/* FNV-1a 32 over size bytes, including any embedded NUL. */
name_hash_t name_hash_n(const void* data, size_t size);

/* FNV-1a 32 over the bytes of name up to its NUL. */
name_hash_t name_hash(const char* name);

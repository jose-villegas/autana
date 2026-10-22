/* Names become stable numeric keys for lookup at initialization or load time. */
#pragma once

#include <stddef.h>
#include <stdint.h>

typedef uint32_t name_hash_t;
typedef uint64_t name_hash64_t;

/* Reserved: bakers refuse stored names that hash to this value. */
#define NAME_HASH_NONE           0U
#define NAME_HASH_OFFSET_BASIS   0x811C9DC5U
#define NAME_HASH_PRIME          0x01000193U

#define NAME_HASH64_OFFSET_BASIS 0xCBF29CE484222325ULL
#define NAME_HASH64_PRIME        0x00000100000001B3ULL

/* FNV-1a 64 folded over size bytes onto seed. Start with NAME_HASH64_OFFSET_BASIS; pass the previous result to continue. */
name_hash64_t name_hash64_n(name_hash64_t seed, const void* data, size_t size);

/* FNV-1a 32 over size bytes, including any embedded NUL. */
name_hash_t name_hash_n(const void* data, size_t size);

/* FNV-1a 32 over the bytes of name up to its NUL. */
name_hash_t name_hash(const char* name);

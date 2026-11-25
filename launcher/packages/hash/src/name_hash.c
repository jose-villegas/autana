#include "hash/name_hash.h"

#include <string.h>

/* Fowler, Noll, Vo: FNV-1a uses unsigned bytes and modulo-32-bit arithmetic. */
name_hash_t
name_hash_seeded_n(name_hash_t seed, const void* data, size_t size) {
    const unsigned char* bytes = data;
    for (size_t i = 0; i < size; i++) {
        seed ^= bytes[i];
        seed *= NAME_HASH_PRIME;
    }
    return seed;
}

name_hash_t
name_hash_n(const void* data, size_t size) {
    return name_hash_seeded_n(NAME_HASH_OFFSET_BASIS, data, size);
}

name_hash_t
name_hash(const char* name) {
    return name_hash_n(name, strlen(name));
}

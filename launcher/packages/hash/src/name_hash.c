#include "hash/name_hash.h"

#include <string.h>

/* Fowler, Noll, Vo: FNV-1a uses unsigned bytes and modulo-32-bit arithmetic. */
name_hash_t
name_hash_n(const void* data, size_t size) {
    const unsigned char* bytes = data;
    name_hash_t hash = NAME_HASH_OFFSET_BASIS;
    for (size_t i = 0; i < size; i++) {
        hash ^= bytes[i];
        hash *= NAME_HASH_PRIME;
    }
    return hash;
}

name_hash_t
name_hash(const char* name) {
    return name_hash_n(name, strlen(name));
}

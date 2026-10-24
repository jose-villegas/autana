/* Fowler, Noll, Vo: FNV-1a uses unsigned bytes and modulo-64-bit arithmetic. */
#include "name_hash/name_hash64.h"

name_hash64_t
name_hash64_n(name_hash64_t seed, const void* data, size_t size) {
    const unsigned char* bytes = data;
    for (size_t i = 0; i < size; i++) {
        seed ^= bytes[i];
        seed *= NAME_HASH64_PRIME;
    }
    return seed;
}

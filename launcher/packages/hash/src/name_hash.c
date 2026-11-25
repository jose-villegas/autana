#include "hash/name_hash.h"

#include <string.h>

name_hash_t
name_hash_n(const void* data, size_t size) {
    return name_hash_seeded_n(NAME_HASH_OFFSET_BASIS, data, size);
}

name_hash_t
name_hash(const char* name) {
    return name_hash_n(name, strlen(name));
}

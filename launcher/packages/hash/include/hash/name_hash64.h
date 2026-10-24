/* Seeded 64-bit name hashes support incremental byte-span fingerprints. */
#pragma once

#include <stddef.h>
#include <stdint.h>

typedef uint64_t name_hash64_t;

#define NAME_HASH64_OFFSET_BASIS 0xCBF29CE484222325ULL
#define NAME_HASH64_PRIME        0x00000100000001B3ULL

/* FNV-1a 64 folded onto seed. Start with NAME_HASH64_OFFSET_BASIS; pass the previous result to continue. */
name_hash64_t name_hash64_n(name_hash64_t seed, const void* data, size_t size);

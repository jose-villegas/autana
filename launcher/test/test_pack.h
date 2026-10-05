/* Writes an asset pack into a test's buffer, the layout docs/assets/README.md
 * gives: test_pack_begin() with the entry count, test_pack_add() for each
 * entry, which returns its zeroed bytes to fill, then test_pack_finish(). */
#pragma once

#include <stdint.h>
#include <string.h>

#include "asset/asset_pack.h"
#include "unity.h"

typedef struct {
    uint8_t* bytes;
    uint32_t capacity, count, added, end;
} test_pack_t;

static inline void
test_pack_put32(uint8_t* at, uint32_t value) {
    for (int i = 0; i < 4; i++) {
        at[i] = (uint8_t)(value >> (8 * i));
    }
}

static inline uint64_t
test_pack_aligned(uint64_t offset) {
    return (offset + 15U) & ~(uint64_t)15U;
}

static inline test_pack_t
test_pack_begin(uint8_t* bytes, uint32_t capacity, uint32_t count) {
    const uint64_t table_end = test_pack_aligned(ASSET_PACK_HEADER_SIZE + ((uint64_t)count * ASSET_PACK_ENTRY_SIZE));
    TEST_ASSERT_TRUE_MESSAGE(table_end <= capacity, "the pack buffer cannot hold the table");
    memset(bytes, 0, capacity);
    return (test_pack_t){bytes, capacity, count, 0, (uint32_t)table_end};
}

static inline uint8_t*
test_pack_add(test_pack_t* pack, const char* name, uint32_t type, uint32_t size) {
    TEST_ASSERT_TRUE_MESSAGE(pack->added < pack->count && strlen(name) < ASSET_NAME_MAX, name);
    const uint64_t end = test_pack_aligned((uint64_t)pack->end + size);
    TEST_ASSERT_TRUE_MESSAGE(end <= pack->capacity, "the pack buffer is full");
    uint8_t* row = pack->bytes + ASSET_PACK_HEADER_SIZE + (pack->added * ASSET_PACK_ENTRY_SIZE);
    memcpy(row, name, strlen(name));
    test_pack_put32(row + 32, type);
    test_pack_put32(row + 36, pack->end);
    test_pack_put32(row + 40, size);
    test_pack_put32(row + 44, 16);
    uint8_t* entry = pack->bytes + pack->end;
    pack->added++;
    pack->end = (uint32_t)end;
    return entry;
}

/* The header, with the CRC over everything after it; returns the pack's size. */
static inline uint32_t
test_pack_finish(test_pack_t* pack) {
    TEST_ASSERT_EQUAL_UINT32(pack->count, pack->added);
    memcpy(pack->bytes, ASSET_PACK_MAGIC, 4);
    test_pack_put32(pack->bytes + 4, ASSET_PACK_VERSION);
    test_pack_put32(pack->bytes + 8, pack->count);
    test_pack_put32(pack->bytes + 12, pack->end);
    test_pack_put32(pack->bytes + 16,
                    asset_crc32(pack->bytes + ASSET_PACK_HEADER_SIZE, pack->end - ASSET_PACK_HEADER_SIZE));
    return pack->end;
}

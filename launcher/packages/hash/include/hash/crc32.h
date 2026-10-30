/*
 * crc32: crc32_update computes incremental RFC 1952 checksums; Kconfig HASH_CRC32_*
 * selects the implementation. Every candidate must equal crc32_update_nibble.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif

#define HASH_CRC32_BIT_LIMIT 0

/* Reference: no table in RAM, two lookups per byte. */
static inline uint32_t
crc32_update_nibble(uint32_t crc, const void* data, size_t size) {
    static const uint32_t nibble[16] = {0x00000000U, 0x1DB71064U, 0x3B6E20C8U, 0x26D930ACU, 0x76DC4190U, 0x6B6B51F4U,
                                        0x4DB26158U, 0x5005713CU, 0xEDB88320U, 0xF00F9344U, 0xD6D6A3E8U, 0xCB61B38CU,
                                        0x9B64C2B0U, 0x86D3D2D4U, 0xA00AE278U, 0xBDBDF21CU};
    const uint8_t* bytes = data;
    crc = ~crc;
    for (size_t i = 0; i < size; i++) {
        crc ^= bytes[i];
        crc = (crc >> 4) ^ nibble[crc & 15U];
        crc = (crc >> 4) ^ nibble[crc & 15U];
    }
    return ~crc;
}

uint32_t crc32_update_slice4(uint32_t crc, const void* data, size_t size);
#ifdef ESP_PLATFORM
uint32_t crc32_update_rom(uint32_t crc, const void* data, size_t size);
#endif

/* zlib's crc32(): 0 starts a checksum; passing the previous result continues it over more bytes. */
/* FAST_PATH(crc32_update, crc32_update_nibble, HASH_CRC32_BIT_LIMIT=0 bit) */
static inline uint32_t
crc32_update(uint32_t crc, const void* data, size_t size) {
#if defined(ESP_PLATFORM) && defined(CONFIG_HASH_CRC32_ROM) && CONFIG_HASH_CRC32_ROM
    return crc32_update_rom(crc, data, size);
#elif defined(ESP_PLATFORM) && defined(CONFIG_HASH_CRC32_SLICE4) && CONFIG_HASH_CRC32_SLICE4
    return crc32_update_slice4(crc, data, size);
#else
    return crc32_update_nibble(crc, data, size);
#endif
}

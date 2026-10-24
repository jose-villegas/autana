/* crc32: the CRC-32 of RFC 1952, which zlib and PNG compute, a nibble at a time, so no 1 KiB table sits in RAM. */
#pragma once

#include <stddef.h>
#include <stdint.h>

/* zlib's crc32(): 0 starts a checksum; passing the previous result continues it over more bytes. */
static inline uint32_t
crc32_update(uint32_t crc, const void* data, size_t size) {
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

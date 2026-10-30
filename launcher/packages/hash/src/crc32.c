/* crc32: reference, sliced and ROM implementations of the RFC 1952 checksum. */
#include "hash/crc32.h"

#include "crc32_tables.inc"

#define HASH_CRC32_BYTE_BITS   8
#define HASH_CRC32_BYTE_MASK   UINT8_MAX
#define HASH_CRC32_SLICE_BYTES 4

uint32_t
crc32_update_slice4(uint32_t crc, const void* data, size_t size) {
    const uint8_t* bytes = data;
    crc = ~crc;
    while (size >= HASH_CRC32_SLICE_BYTES) {
        for (size_t lane = 0; lane < HASH_CRC32_SLICE_BYTES; lane++) {
            crc ^= (uint32_t)bytes[lane] << (lane * HASH_CRC32_BYTE_BITS);
        }
        crc = crc32_slice[3][crc & HASH_CRC32_BYTE_MASK]
              ^ crc32_slice[2][(crc >> HASH_CRC32_BYTE_BITS) & HASH_CRC32_BYTE_MASK]
              ^ crc32_slice[1][(crc >> (2 * HASH_CRC32_BYTE_BITS)) & HASH_CRC32_BYTE_MASK]
              ^ crc32_slice[0][crc >> (3 * HASH_CRC32_BYTE_BITS)];
        bytes += HASH_CRC32_SLICE_BYTES;
        size -= HASH_CRC32_SLICE_BYTES;
    }
    while (size-- > 0) {
        crc = (crc >> HASH_CRC32_BYTE_BITS) ^ crc32_slice[0][(crc ^ *bytes++) & HASH_CRC32_BYTE_MASK];
    }
    return ~crc;
}

#ifdef ESP_PLATFORM
/* esp32s3.rom.api.ld provides this entry, whose entry/exit complements match zlib. */
uint32_t esp_rom_crc32_le(uint32_t crc, const uint8_t* data, uint32_t size);

uint32_t
crc32_update_rom(uint32_t crc, const void* data, size_t size) {
    return size == 0 ? crc : esp_rom_crc32_le(crc, data, (uint32_t)size);
}
#endif

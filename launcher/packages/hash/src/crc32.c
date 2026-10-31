/* crc32: ROM implementation of the RFC 1952 checksum. */
#include "hash/crc32.h"

#ifdef ESP_PLATFORM
/* esp32s3.rom.api.ld provides this entry, whose entry/exit complements match zlib. */
uint32_t esp_rom_crc32_le(uint32_t crc, const uint8_t* data, uint32_t size);

uint32_t
crc32_update_rom(uint32_t crc, const void* data, size_t size) {
    return size == 0 ? crc : esp_rom_crc32_le(crc, data, (uint32_t)size);
}
#endif

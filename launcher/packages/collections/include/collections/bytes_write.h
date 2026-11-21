/* bytes_write: little-endian fields written at any alignment for pack and wire formats. */
#pragma once

#include <limits.h>
#include <stdint.h>

static inline void
bytes_write_u16(uint8_t* at, uint16_t value) {
    at[0] = (uint8_t)value;
    at[1] = (uint8_t)(value >> CHAR_BIT);
}

static inline void
bytes_write_u32(uint8_t* at, uint32_t value) {
    bytes_write_u16(at, (uint16_t)value);
    bytes_write_u16(at + sizeof(uint16_t), (uint16_t)(value >> (sizeof(uint16_t) * CHAR_BIT)));
}

static inline void
bytes_write_u64(uint8_t* at, uint64_t value) {
    bytes_write_u32(at, (uint32_t)value);
    bytes_write_u32(at + sizeof(uint32_t), (uint32_t)(value >> (sizeof(uint32_t) * CHAR_BIT)));
}
